#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>
#include <WebView2.h>
#include <gdiplus.h>

#include "common/ipc.h"
#include "window_frame.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using nlohmann::json;
#define EUPHONY_WIDE_IMPL(x) L##x
#define EUPHONY_WIDE(x) EUPHONY_WIDE_IMPL(x)
constexpr wchar_t window_class[] = L"Euphony.Settings.Window." EUPHONY_WIDE(EUPHONY_VERSION);
constexpr UINT response_message = WM_APP + 1;
constexpr UINT_PTR self_test_timer = 1;

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid UTF-16 text");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::filesystem::path executable_directory() {
    std::wstring value(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!size || size == value.size()) throw std::runtime_error("Cannot locate Euphony executable");
    value.resize(size);
    return std::filesystem::path(value).parent_path();
}

std::filesystem::path profile_directory() {
    const auto directory = euphony::user_directory() / L"webview";
    std::filesystem::create_directories(directory);
    return directory;
}

class SettingsApp {
public:
    explicit SettingsApp(HINSTANCE instance, std::filesystem::path test_output = {})
        : instance_(instance), test_output_(std::move(test_output)) {}
    ~SettingsApp() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            requests_.clear();
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        webview_.Reset();
        if (controller_) controller_->Close();
        controller_.Reset();
        environment_.Reset();
        if (loader_) FreeLibrary(loader_);
    }

    int run() {
        if (testing()) {
            if (!test_output_.is_absolute()) throw std::runtime_error("Self-test output directory must be absolute");
            if (GetEnvironmentVariableW(L"EUPHONY_USER_DIR", nullptr, 0) <= 1 ||
                GetEnvironmentVariableW(L"EUPHONY_INSTANCE", nullptr, 0) <= 1)
                throw std::runtime_error("Self-test requires isolated EUPHONY_USER_DIR and EUPHONY_INSTANCE");
            if (!euphony::user_directory().is_absolute())
                throw std::runtime_error("Self-test EUPHONY_USER_DIR must be absolute");
            std::filesystem::create_directories(test_output_);
            test_started_ = GetTickCount64();
        }
        WNDCLASSW wc{};
        wc.hInstance = instance_;
        wc.lpszClassName = window_class;
        wc.lpfnWndProc = window_proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_EUPHONY));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&wc)) return 1;
        const UINT dpi = GetDpiForSystem();
        MONITORINFO monitor{sizeof(monitor)};
        POINT cursor{};
        GetCursorPos(&cursor);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int width = std::min(MulDiv(1080, dpi, 96),
            static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - MulDiv(32, dpi, 96));
        const int height = std::min(MulDiv(810, dpi, 96),
            static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - MulDiv(32, dpi, 96));
        window_ = CreateWindowExW(0, window_class, L"Euphony · 设置",
            WS_OVERLAPPEDWINDOW, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height,
            nullptr, nullptr, instance_, this);
        if (!window_) return 1;
        frame_.initialize(instance_, window_);
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_EUPHONY), IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED)));
        ShowWindow(window_, testing() ? SW_HIDE : SW_SHOW);
        UpdateWindow(window_);
        if (testing()) SetTimer(window_, self_test_timer, 200, nullptr);
        worker_ = std::thread([this] { service_requests(); });
        initialize_webview();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    HINSTANCE instance_{};
    HWND window_{};
    WindowFrame frame_;
    HMODULE loader_{};
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    bool local_navigation_pending_ = false;
    bool document_ready_ = false;
    std::wstring local_document_uri_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<json> requests_;
    std::deque<json> responses_;
    bool stopping_ = false;
    int exit_code_ = 0;
    std::filesystem::path test_output_;
    ULONGLONG test_started_ = 0;
    bool test_navigation_ = false;
    bool test_evaluating_ = false;
    bool test_finished_ = false;
    std::string test_phase_ = "window";
    std::string test_message_source_;
    std::string test_navigation_uri_;
    size_t test_messages_ = 0;
    json test_config_;
    json test_dom_;
    ComPtr<IStream> test_image_stream_;

    bool testing() const { return !test_output_.empty(); }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        SettingsApp* app = reinterpret_cast<SettingsApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<SettingsApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            app->frame_.attach(window);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) {
            switch (message) {
            case WM_NCCALCSIZE:
                return 0;
            case WM_NCHITTEST:
                return app->frame_.hit_test(lparam);
            case WM_ERASEBKGND:
                return 1;
            case WM_DRAWITEM:
                if (app->frame_.draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) return TRUE;
                break;
            case WM_COMMAND:
                if (app->frame_.command(wparam)) return 0;
                break;
            case WM_SIZE:
                app->resize();
                return 0;
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
                app->resize();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                app->frame_.minmax(*info);
                return 0;
            }
            case response_message:
                app->deliver_responses();
                return 0;
            case WM_TIMER:
                if (wparam == self_test_timer && app->testing()) app->poll_self_test();
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC context = BeginPaint(window, &paint);
                app->frame_.paint(context);
                if (!app->document_ready_) {
                    RECT bounds = app->frame_.content();
                    auto background = CreateSolidBrush(app->frame_.background());
                    FillRect(context, &bounds, background); DeleteObject(background);
                }
                EndPaint(window, &paint);
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
            case WM_DESTROY: {
                std::lock_guard lock(app->mutex_);
                app->stopping_ = true;
                app->wake_.notify_one();
                PostQuitMessage(app->exit_code_);
                return 0;
            }
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void fail(const std::wstring& detail, HRESULT code = S_OK) {
        std::wstring message = detail;
        if (FAILED(code)) {
            std::wostringstream result;
            result << L"\n\n错误代码：0x" << std::hex << std::uppercase << static_cast<unsigned long>(code);
            message += result.str();
        }
        exit_code_ = 1;
        if (testing()) {
            finish_self_test({{"ok", false}, {"error", utf8(message)}});
            return;
        }
        MessageBoxW(window_, message.c_str(), L"Euphony 设置", MB_OK | MB_ICONERROR);
        DestroyWindow(window_);
    }

    void initialize_webview() {
        try {
            const auto loader_path = executable_directory() / L"WebView2Loader.dll";
            loader_ = LoadLibraryExW(loader_path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!loader_) {
                fail(L"找不到 WebView2Loader.dll。请确认 Euphony 安装目录包含此文件。",
                     HRESULT_FROM_WIN32(GetLastError()));
                return;
            }
            using CreateEnvironment = HRESULT(STDAPICALLTYPE *)(PCWSTR, PCWSTR,
                ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
            const auto create = reinterpret_cast<CreateEnvironment>(
                GetProcAddress(loader_, "CreateCoreWebView2EnvironmentWithOptions"));
            if (!create) {
                fail(L"WebView2Loader.dll 缺少环境创建接口。请重新安装 Euphony。",
                     HRESULT_FROM_WIN32(GetLastError()));
                return;
            }
            const auto profile = profile_directory();
            test_phase_ = "environment_requested";
            const HRESULT result = create(nullptr, profile.c_str(), nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [this](HRESULT status, ICoreWebView2Environment* environment) -> HRESULT {
                        if (stopping()) return S_OK;
                        if (FAILED(status) || !environment) {
                            fail(L"无法启动 Microsoft Edge WebView2。请安装 Microsoft 官方 WebView2 Runtime 后重新打开设置。", status);
                            return S_OK;
                        }
                        environment_ = environment;
                        test_phase_ = "controller_requested";
                        const HRESULT created = environment_->CreateCoreWebView2Controller(window_,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [this](HRESULT controller_status, ICoreWebView2Controller* controller) -> HRESULT {
                                    if (stopping()) return S_OK;
                                    if (FAILED(controller_status) || !controller) {
                                        fail(L"无法创建 WebView2 设置窗口。", controller_status);
                                        return S_OK;
                                    }
                                    controller_ = controller;
                                    test_phase_ = "controller_created";
                                    controller_->get_CoreWebView2(&webview_);
                                    if (!webview_) {
                                        fail(L"WebView2 未返回可用的浏览器。", E_FAIL);
                                        return S_OK;
                                    }
                                    configure_webview();
                                    return S_OK;
                                }).Get());
                        if (FAILED(created)) fail(L"无法创建 WebView2 控件。", created);
                        return S_OK;
                    }).Get());
            if (FAILED(result)) fail(L"无法初始化 WebView2。请安装 Microsoft 官方 WebView2 Runtime。", result);
        } catch (const std::exception& error) {
            fail(L"打开设置时发生错误：\n" + wide(error.what()));
        }
    }

    bool stopping() {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    void configure_webview() {
        ComPtr<ICoreWebView2Settings> settings;
        webview_->get_Settings(&settings);
        if (settings) {
            settings->put_IsWebMessageEnabled(TRUE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            ComPtr<ICoreWebView2Settings3> accelerator_settings;
            if (SUCCEEDED(settings.As(&accelerator_settings)))
                accelerator_settings->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        EventRegistrationToken token{};
        // The only document is local HTML passed to NavigateToString (about:blank).
        // No remote page can navigate into this WebView or reach the native bridge.
        webview_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                PWSTR uri = nullptr;
                args->get_Uri(&uri);
                const std::wstring destination = uri ? uri : L"";
                // Current runtimes expose NavigateToString's internal data URL
                // to NavigationStarting even though the page origin is about:blank.
                const bool initial_local = local_navigation_pending_ &&
                    destination.starts_with(L"data:text/html;charset=utf-8;base64,");
                const bool trusted = destination == L"about:blank" || initial_local;
                if (initial_local) {
                    local_document_uri_ = destination;
                    local_navigation_pending_ = false;
                }
                if (uri) test_navigation_uri_ = utf8(destination.substr(0, 80));
                CoTaskMemFree(uri);
                if (!trusted) args->put_Cancel(TRUE);
                return S_OK;
            }).Get(), &token);
        webview_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), &token);
        webview_->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            }).Get(), &token);
        webview_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                COREWEBVIEW2_WEB_ERROR_STATUS error = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_WebErrorStatus(&error);
                if (!success && error != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED && !stopping())
                    fail(L"WebView2 无法显示本地设置页面。请重新打开 Euphony 设置。");
                if (success) {
                    document_ready_=true; controller_->put_IsVisible(TRUE);
                    InvalidateRect(window_,nullptr,FALSE); test_navigation_ = true;
                }
                if (success) test_phase_ = "document_loaded";
                return S_OK;
            }).Get(), &token);
        webview_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                PWSTR source = nullptr;
                args->get_Source(&source);
                ++test_messages_;
                if (source) test_message_source_ = utf8(std::wstring(source).substr(0, 80));
                const bool trusted = source && (std::wstring(source) == L"about:blank" ||
                    (!local_document_uri_.empty() && std::wstring(source) == local_document_uri_));
                CoTaskMemFree(source);
                if (!trusted) return S_OK;
                PWSTR raw = nullptr;
                if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
                std::string text;
                try { text = utf8(raw); } catch (...) { CoTaskMemFree(raw); return S_OK; }
                CoTaskMemFree(raw);
                if (text.size() > 64 * 1024) return S_OK;
                try { handle_request(json::parse(text)); }
                catch (const std::exception& error) {
                    send({{"id", 0}, {"op", "error"}, {"result", {{"ok", false}, {"error", error.what()}}}});
                }
                return S_OK;
            }).Get(), &token);
        try {
            std::ifstream file(executable_directory() / L"settings.html", std::ios::binary);
            if (!file) throw std::runtime_error("settings.html is missing from the Euphony installation directory");
            const std::string html((std::istreambuf_iterator<char>(file)), {});
            const auto content = wide(html);
            if (content.empty() || content.size() * sizeof(wchar_t) > 2 * 1024 * 1024)
                throw std::runtime_error("settings.html is empty or exceeds the WebView2 limit");
            resize();
            controller_->put_IsVisible(FALSE);
            test_phase_ = "navigation_requested";
            local_navigation_pending_ = true;
            const HRESULT result = webview_->NavigateToString(content.c_str());
            if (FAILED(result)) fail(L"无法打开本地设置页面。", result);
        } catch (const std::exception& error) {
            fail(L"无法读取设置页面：\n" + wide(error.what()));
        }
    }

    void resize() {
        frame_.resize();
        if (!controller_) return;
        RECT bounds = frame_.content();
        controller_->put_Bounds(bounds);
    }

    void send(const json& response) {
        if (webview_ && !stopping()) webview_->PostWebMessageAsJson(wide(response.dump()).c_str());
    }

    void handle_request(const json& request) {
        if (!request.is_object() || !request.contains("id") || !request["id"].is_number_unsigned() ||
            !request.contains("op") || !request["op"].is_string()) return;
        const std::string operation = request["op"];
        if (operation == "preview_theme") {
            if (request.contains("dark") && request["dark"].is_boolean()) frame_.set_dark(request["dark"].get<bool>());
            return;
        }
        if (operation == "pick_path") {
            pick_path(request);
            return;
        }
        if (operation != "get_config" && operation != "set_config") return;
        if (operation == "set_config" && (!request.contains("config") || !request["config"].is_object())) return;
        if (testing() && operation == "set_config") {
            finish_self_test({{"ok", false}, {"error", "Runtime self-test must not change configuration"}});
            return;
        }
        bool full = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            full = requests_.size() >= 8;
            if (!full) requests_.push_back(request);
        }
        if (full) send({{"id", request["id"]}, {"op", operation}, {"result", {{"ok", false}, {"error", "设置服务正在处理请求，请稍后重试。"}}}});
        else wake_.notify_one();
    }

    void service_requests() {
        while (true) {
            json request;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
                if (stopping_) return;
                request = std::move(requests_.front());
                requests_.pop_front();
            }
            json result;
            try {
                if (!euphony::ensure_broker()) throw std::runtime_error("无法启动 Euphony 输入服务，请检查程序安装目录。");
                euphony::IpcClient client(15000);
                json message = {{"version", 1}, {"op", request["op"]}};
                if (request["op"] == "set_config") message["config"] = request["config"];
                result = client.call(message);
            } catch (const std::exception& error) {
                result = {{"ok", false}, {"error", error.what()}};
            }
            {
                std::lock_guard lock(mutex_);
                if (stopping_) return;
                responses_.push_back({{"id", request["id"]}, {"op", request["op"]}, {"result", std::move(result)}});
            }
            PostMessageW(window_, response_message, 0, 0);
        }
    }

    void deliver_responses() {
        std::deque<json> responses;
        {
            std::lock_guard lock(mutex_);
            responses.swap(responses_);
        }
        for (const auto& response : responses) {
            if (testing() && response.value("op", "") == "get_config") {
                const auto& result = response.at("result");
                if (!result.value("ok", false)) {
                    finish_self_test({{"ok", false}, {"error", result.value("error", "Configuration bridge failed")}});
                    return;
                }
                test_config_ = result.at("config");
            }
            send(response);
        }
    }

    void poll_self_test() {
        if (test_finished_) return;
        if (GetTickCount64() - test_started_ > 30000) {
            finish_self_test({{"ok", false}, {"error", "WebView2 runtime self-test timed out"},
                {"phase", test_phase_}, {"environment", bool(environment_)}, {"controller", bool(controller_)},
                {"webview", bool(webview_)}, {"navigation", test_navigation_}, {"bridge_config", !test_config_.is_null()},
                {"web_messages", test_messages_}, {"message_source", test_message_source_}, {"navigation_uri", test_navigation_uri_}});
            return;
        }
        if (!webview_ || !test_navigation_ || test_config_.is_null() || test_evaluating_) return;
        test_evaluating_ = true;
        // This evaluates the application's own page; it does not send set_config.
        constexpr wchar_t script[] = LR"JS((() => {
            const fields = document.getElementById('fields');
            if (!fields || fields.disabled) return {ready:false};
            const ids = ['input-mode','shuangpin-scheme','page-size','candidate-layout','model-candidate-color','user-candidate-color','invalid-input','learning',
                'model-enabled','prefer-unigram-single-syllable','user-words-exact-only','user-words-enabled','user-words-first','model-directory','core-library','dictionary-path','language-model-path','use-surrounding',
                'history-characters','context-slots','chinese-punctuation','full-width','full-width-digits','full-width-symbols','lowercase-ascii','normalize-v','separators',
                'max-pinyin-chars','max-context-length','max-history-length','max-pinyin-length',
                'slack-interval','min-accept-context','trial-ratio','decay-alpha','decay-lambda'];
            const missing = ids.filter(id => !document.getElementById(id));
            const theme = document.getElementById('theme');
            theme.value = 'dark'; theme.dispatchEvent(new Event('change', {bubbles:true}));
            const darkTheme = document.documentElement.dataset.theme === 'dark' &&
                getComputedStyle(document.body).backgroundColor === 'rgb(33, 28, 40)' &&
                getComputedStyle(document.querySelector('section')).backgroundColor === 'rgb(44, 37, 53)';
            theme.value = 'light'; theme.dispatchEvent(new Event('change', {bubbles:true}));
            const lightTheme = document.documentElement.dataset.theme === 'light' &&
                getComputedStyle(document.body).backgroundColor === 'rgb(247, 248, 245)';
            theme.value = 'system'; theme.dispatchEvent(new Event('change', {bubbles:true}));
            const systemTheme = document.documentElement.dataset.theme ===
                (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
            document.getElementById('revert').click();
            const themeReverted = document.documentElement.dataset.theme === (theme.value === 'system' ? (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light') : theme.value);
            const mode = document.getElementById('input-mode');
            const originalMode = mode.value;
            mode.value = originalMode === 'pinyin' ? 'shuangpin' : 'pinyin';
            mode.dispatchEvent(new Event('change', {bubbles:true}));
            const dirtyState = !document.getElementById('save').disabled;
            const schemeToggle = document.getElementById('shuangpin-scheme').disabled === (mode.value !== 'shuangpin');
            document.getElementById('revert').click();
            const layout = document.getElementById('candidate-layout');
            const originalLayout = layout.value;
            layout.value = originalLayout === 'vertical' ? 'horizontal' : 'vertical';
            layout.dispatchEvent(new Event('change', {bubbles:true}));
            const layoutDirtyState = !document.getElementById('save').disabled;
            document.getElementById('revert').click();
            const unigram = document.getElementById('prefer-unigram-single-syllable');
            const originalUnigram = unigram.checked;
            unigram.checked = !originalUnigram;
            unigram.dispatchEvent(new Event('change', {bubbles:true}));
            const unigramDirtyState = !document.getElementById('save').disabled;
            document.getElementById('revert').click();
            const exactOnly = document.getElementById('user-words-exact-only');
            exactOnly.checked = !exactOnly.checked;
            exactOnly.dispatchEvent(new Event('change', {bubbles:true}));
            let userWordsDirtyState = !document.getElementById('save').disabled;
            document.getElementById('revert').click();
            for (const id of ['user-words-enabled','user-words-first']) {
                const toggle = document.getElementById(id);
                toggle.checked = !toggle.checked;
                toggle.dispatchEvent(new Event('change', {bubbles:true}));
                userWordsDirtyState = userWordsDirtyState && !document.getElementById('save').disabled;
                document.getElementById('revert').click();
            }
            const languageModel = document.getElementById('language-model-path');
            const originalLanguageModel = languageModel.value;
            languageModel.value = originalLanguageModel + '.self-test';
            languageModel.dispatchEvent(new Event('input', {bubbles:true}));
            const languageModelDirtyState = !document.getElementById('save').disabled;
            document.getElementById('revert').click();
            const pageSize = document.getElementById('page-size');
            pageSize.value = '2';
            pageSize.dispatchEvent(new Event('input', {bubbles:true}));
            document.getElementById('save').click();
            const validation = pageSize.classList.contains('field-error') &&
                document.getElementById('error').classList.contains('visible');
            document.getElementById('revert').click();
            const sidebarTop = document.querySelector('aside').getBoundingClientRect().top;
            const scrollToSection = id => {
                window.scrollTo({top:document.getElementById(id).getBoundingClientRect().top + scrollY - 20,behavior:'instant'});
                window.dispatchEvent(new Event('scroll'));
                return document.querySelector('nav a.active').getAttribute('href') === '#' + id &&
                    document.querySelectorAll('nav a[aria-current="location"]').length === 1;
            };
            const scrollForward = scrollToSection('resources') && scrollToSection('privacy');
            window.scrollTo({top:document.documentElement.scrollHeight,behavior:'instant'});
            window.dispatchEvent(new Event('scroll'));
            const scrollBottom = document.querySelector('nav a.active').getAttribute('href') === '#advanced';
            const scrollBackward = scrollToSection('resources');
            const sidebarSticky = Math.abs(document.querySelector('aside').getBoundingClientRect().top - sidebarTop) < 1;
            window.scrollTo({top:0,left:0,behavior:'instant'});
            window.dispatchEvent(new Event('scroll'));
            const scrollTop = document.querySelector('nav a.active').getAttribute('href') === '#input';
            const viewport = {width:innerWidth,height:innerHeight};
            const footer = document.querySelector('footer').getBoundingClientRect();
            return {ready:true,missing,scroll_forward:scrollForward,scroll_backward:scrollBackward,scroll_bottom:scrollBottom,scroll_top:scrollTop,sidebar_sticky:sidebarSticky,dark_theme:darkTheme,light_theme:lightTheme,system_theme:systemTheme,theme_reverted:themeReverted,control_count:document.querySelectorAll('[data-key]').length,
                schemes:document.getElementById('shuangpin-scheme').options.length,
                dirty_state:dirtyState,scheme_toggle:schemeToggle,validation,
                layout_dirty_state:layoutDirtyState,candidate_layout:layout.value,
                model_candidate_color:document.getElementById('model-candidate-color').value.toUpperCase(),
                user_candidate_color:document.getElementById('user-candidate-color').value.toUpperCase(),
                unigram_dirty_state:unigramDirtyState,language_model_dirty_state:languageModelDirtyState,
                language_model_picker:!!document.querySelector('[data-pick="language_model_path"]'),
                reverted:document.getElementById('save').disabled,
                model_directory:document.getElementById('model-directory').value,
                core_library:document.getElementById('core-library').value,
                dictionary_path:document.getElementById('dictionary-path').value,
                language_model_path:languageModel.value,
                prefer_unigram_single_syllable:unigram.checked,
                user_words_exact_only:exactOnly.checked,user_words_dirty_state:userWordsDirtyState,
                user_words_enabled:document.getElementById('user-words-enabled').checked,
                user_words_first:document.getElementById('user-words-first').checked,
                page_size:Number(pageSize.value),input_mode:mode.value,
                shuangpin_scheme:document.getElementById('shuangpin-scheme').value,
                model_enabled:document.getElementById('model-enabled').checked,
                full_width:document.getElementById('full-width').checked,
                full_width_digits:document.getElementById('full-width-digits').checked,full_width_symbols:document.getElementById('full-width-symbols').checked,
                chinese_punctuation:document.getElementById('chinese-punctuation').checked,
                viewport,footer_visible:footer.top >= 0 && footer.bottom <= innerHeight,
                model_status:document.getElementById('model-status').textContent,
                dictionary_status:document.getElementById('dictionary-status').textContent,
                language_model_status:document.getElementById('language-model-status').textContent};
        })())JS";
        const HRESULT result = webview_->ExecuteScript(script,
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [this](HRESULT status, PCWSTR raw) -> HRESULT {
                    if (test_finished_ || stopping()) return S_OK;
                    if (FAILED(status) || !raw) {
                        finish_self_test({{"ok", false}, {"error", "Cannot inspect the WebView2 settings document"}});
                        return S_OK;
                    }
                    try {
                        test_dom_ = json::parse(utf8(raw));
                        if (!test_dom_.value("ready", false)) {
                            test_evaluating_ = false;
                            return S_OK;
                        }
                        bool valid = test_dom_.at("missing").empty() &&
                            test_dom_.value("scroll_forward", false) && test_dom_.value("scroll_backward", false) &&
                            test_dom_.value("scroll_bottom", false) && test_dom_.value("scroll_top", false) &&
                            test_dom_.value("sidebar_sticky", false) && test_dom_.value("dark_theme", false) && test_dom_.value("light_theme", false) &&
                            test_dom_.value("system_theme", false) && test_dom_.value("theme_reverted", false) &&
                            test_dom_.value("schemes", 0) == 7 && test_dom_.value("dirty_state", false) &&
                            test_dom_.value("unigram_dirty_state", false) && test_dom_.value("language_model_dirty_state", false) &&
                            test_dom_.value("user_words_dirty_state", false) &&
                            test_dom_.value("layout_dirty_state", false) &&
                            test_dom_.value("language_model_picker", false) &&
                            test_dom_.value("scheme_toggle", false) && test_dom_.value("validation", false) &&
                            test_dom_.value("reverted", false) && test_dom_.value("footer_visible", false);
                        for (const char* key : {"model_directory", "core_library", "dictionary_path", "language_model_path", "prefer_unigram_single_syllable", "user_words_exact_only", "user_words_enabled", "user_words_first", "page_size", "candidate_layout", "model_candidate_color", "user_candidate_color", "input_mode", "shuangpin_scheme", "model_enabled", "full_width", "full_width_digits", "full_width_symbols", "chinese_punctuation"})
                            valid = valid && test_dom_.at(key) == test_config_.at(key);
                        if (!valid) {
                            finish_self_test({{"ok", false}, {"error", "Settings DOM or configuration binding check failed"}, {"dom", test_dom_}});
                            return S_OK;
                        }
                        capture_self_test();
                    } catch (const std::exception& error) {
                        finish_self_test({{"ok", false}, {"error", error.what()}});
                    }
                    return S_OK;
                }).Get());
        if (FAILED(result)) finish_self_test({{"ok", false}, {"error", "WebView2 ExecuteScript failed"}});
    }

    void capture_self_test() {
        HRESULT status = CreateStreamOnHGlobal(nullptr, TRUE, &test_image_stream_);
        if (FAILED(status)) {
            finish_self_test({{"ok", false}, {"error", "Cannot create screenshot stream"}});
            return;
        }
        status = webview_->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, test_image_stream_.Get(),
            Callback<ICoreWebView2CapturePreviewCompletedHandler>([this](HRESULT captured) -> HRESULT {
                if (test_finished_ || stopping()) return S_OK;
                try {
                    if (FAILED(captured)) throw std::runtime_error("WebView2 CapturePreview failed");
                    STATSTG statistics{};
                    if (FAILED(test_image_stream_->Stat(&statistics, STATFLAG_NONAME)) ||
                        statistics.cbSize.HighPart || statistics.cbSize.LowPart > 32 * 1024 * 1024 || !statistics.cbSize.LowPart)
                        throw std::runtime_error("Invalid screenshot stream");
                    LARGE_INTEGER origin{};
                    if (FAILED(test_image_stream_->Seek(origin, STREAM_SEEK_SET, nullptr)))
                        throw std::runtime_error("Cannot rewind screenshot stream");
                    std::vector<char> image(statistics.cbSize.LowPart);
                    ULONG read = 0;
                    if (FAILED(test_image_stream_->Read(image.data(), static_cast<ULONG>(image.size()), &read)) || read != image.size())
                        throw std::runtime_error("Cannot read screenshot stream");
                    const auto screenshot = test_output_ / L"settings.png";
                    {
                        std::ofstream file(screenshot, std::ios::binary | std::ios::trunc);
                        file.write(image.data(), static_cast<std::streamsize>(image.size()));
                        file.flush();
                        if (!file) throw std::runtime_error("Cannot write settings screenshot");
                    }
                    PWSTR runtime = nullptr;
                    environment_->get_BrowserVersionString(&runtime);
                    const std::string version = runtime ? utf8(runtime) : "unknown";
                    CoTaskMemFree(runtime);
                    const auto frame_report = inspect_frame();
                    if (!frame_report.value("ok", false)) throw std::runtime_error("Custom window frame or icon validation failed");
                    const auto window_image = test_output_ / L"window.png";
                    capture_frame(screenshot, window_image);
                    finish_self_test({{"ok", true}, {"bridge_load", true}, {"config_unchanged", true},
                        {"runtime", version}, {"screenshot", utf8(screenshot.wstring())},
                        {"window_screenshot", utf8(window_image.wstring())}, {"frame", frame_report}, {"dom", test_dom_}});
                } catch (const std::exception& error) {
                    finish_self_test({{"ok", false}, {"error", error.what()}});
                }
                return S_OK;
            }).Get());
        if (FAILED(status)) finish_self_test({{"ok", false}, {"error", "Cannot start WebView2 CapturePreview"}});
    }

    json inspect_frame() const {
        RECT client{}, outer{}, web{}; GetClientRect(window_, &client); GetWindowRect(window_, &outer);
        controller_->get_Bounds(&web);
        const auto expected = frame_.content();
        auto hit = [&](int x, int y) {
            POINT point{x, y}; ClientToScreen(window_, &point);
            return SendMessageW(window_, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        const bool no_native_frame = client.right == outer.right - outer.left && client.bottom == outer.bottom - outer.top;
        const bool drag = hit(MulDiv(100, GetDpiForWindow(window_), 96), MulDiv(21, GetDpiForWindow(window_), 96)) == HTCAPTION;
        const bool sizing = hit(1, client.bottom / 2) == HTLEFT;
        POINT edge{MulDiv(3, GetDpiForWindow(window_), 96), client.bottom / 2}; ClientToScreen(window_, &edge);
        bool resize_uncovered = true;
        for (auto child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT child_bounds{}; GetWindowRect(child, &child_bounds);
            if (PtInRect(&child_bounds, edge)) resize_uncovered = false;
        }
        const bool content = EqualRect(&web, &expected) && expected.left >= MulDiv(6, GetDpiForWindow(window_), 96) &&
            expected.top > 0 && hit(client.right / 2, expected.top + 30) == HTCLIENT;
        const bool icons = SendMessageW(window_, WM_GETICON, ICON_SMALL, 0) != 0 && GetClassLongPtrW(window_, GCLP_HICON) != 0;
        return {{"ok", no_native_frame && drag && sizing && resize_uncovered && content && icons && frame_.controls_ready()},
            {"custom_frame", no_native_frame}, {"drag_hit_test", drag}, {"resize_hit_test", sizing},
            {"resize_band_uncovered", resize_uncovered}, {"webview_below_titlebar", content},
            {"caption_buttons", frame_.controls_ready()}, {"window_icons", icons}};
    }

    void capture_frame(const std::filesystem::path& content, const std::filesystem::path& output) const {
        Gdiplus::GdiplusStartupInput startup;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) throw std::runtime_error("Cannot initialize frame screenshot encoder");
        bool saved = false;
        {
            RECT client{}; GetClientRect(window_, &client);
            Gdiplus::Bitmap image(client.right, client.bottom, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&image);
            const auto background=frame_.background();
            graphics.Clear(Gdiplus::Color(255,GetRValue(background),GetGValue(background),GetBValue(background)));
            Gdiplus::Image page(content.c_str());
            const auto bounds = frame_.content();
            graphics.DrawImage(&page, static_cast<INT>(bounds.left), static_cast<INT>(bounds.top),
                static_cast<INT>(bounds.right - bounds.left), static_cast<INT>(bounds.bottom - bounds.top));
            const auto dc = graphics.GetHDC(); frame_.paint(dc); frame_.paint_controls(dc); graphics.ReleaseHDC(dc);
            const CLSID png_encoder{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            saved = image.Save(output.c_str(), &png_encoder, nullptr) == Gdiplus::Ok;
        }
        Gdiplus::GdiplusShutdown(token);
        if (!saved) throw std::runtime_error("Cannot save native window frame preview");
    }

    void finish_self_test(json result) {
        if (test_finished_) return;
        test_finished_ = true;
        KillTimer(window_, self_test_timer);
        exit_code_ = result.value("ok", false) ? 0 : 1;
        result["version"] = 1;
        try {
            std::ofstream file(test_output_ / L"result.json", std::ios::binary | std::ios::trunc);
            file << result.dump(2) << '\n';
            file.flush();
            if (!file) exit_code_ = 1;
        } catch (...) { exit_code_ = 1; }
        DestroyWindow(window_);
    }

    void pick_path(const json& request) {
        const std::string field = request.value("field", "");
        const bool folder = field == "model_directory";
        if (!folder && field != "core_library" && field != "dictionary_path" && field != "language_model_path") return;
        json result;
        ComPtr<IFileOpenDialog> dialog;
        HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(status)) {
            FILEOPENDIALOGOPTIONS options{};
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
            dialog->SetTitle(folder ? L"选择 Phono 模型目录" : field == "core_library" ? L"选择 phono-core 动态链接库" :
                field == "language_model_path" ? L"选择 libime KenLM 语言模型" : L"选择 libime 来源词库");
            const COMDLG_FILTERSPEC dll_filters[] = {{L"动态链接库 (*.dll)", L"*.dll"}};
            const COMDLG_FILTERSPEC dictionary_filters[] = {{L"libime 来源词库 (*.tsv;*.txt)", L"*.tsv;*.txt"}, {L"所有文件", L"*.*"}};
            const COMDLG_FILTERSPEC language_model_filters[] = {{L"KenLM 模型 (*.lm;*.arpa)", L"*.lm;*.arpa"}, {L"所有文件", L"*.*"}};
            if (field == "core_library") dialog->SetFileTypes(1, dll_filters);
            if (field == "dictionary_path") dialog->SetFileTypes(2, dictionary_filters);
            if (field == "language_model_path") dialog->SetFileTypes(2, language_model_filters);
            const auto current = request.value("current", "");
            if (!current.empty()) {
                const auto current_path = std::filesystem::path(wide(current));
                const auto initial = folder ? current_path : current_path.parent_path();
                ComPtr<IShellItem> item;
                if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item)))) dialog->SetFolder(item.Get());
            }
            status = dialog->Show(window_);
            if (SUCCEEDED(status)) {
                ComPtr<IShellItem> item;
                status = dialog->GetResult(&item);
                if (SUCCEEDED(status)) {
                    PWSTR raw = nullptr;
                    status = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
                    if (SUCCEEDED(status)) {
                        result = {{"ok", true}, {"field", field}, {"path", utf8(raw)}};
                        CoTaskMemFree(raw);
                    }
                }
            }
        }
        if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) result = {{"ok", true}, {"cancelled", true}};
        else if (FAILED(status)) result = {{"ok", false}, {"error", "无法打开系统文件选择器。"}};
        send({{"id", request["id"]}, {"op", "pick_path"}, {"result", std::move(result)}});
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    std::filesystem::path test_output;
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) { CoUninitialize(); return 1; }
    if (argument_count == 3 && std::wstring(arguments[1]) == L"--self-test") test_output = arguments[2];
    else if (argument_count != 1) {
        LocalFree(arguments);
        CoUninitialize();
        return 2;
    }
    LocalFree(arguments);
    HANDLE mutex = nullptr;
    if (test_output.empty()) {
        const std::wstring mutex_name = L"Local\\Euphony.Settings-" EUPHONY_WIDE(EUPHONY_VERSION) L"-" + euphony::instance_name();
        mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    }
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const HWND existing = FindWindowW(window_class, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }
    int result = 1;
    try {
        SettingsApp application(instance, test_output);
        result = application.run();
    } catch (const std::exception& error) {
        const std::wstring message = L"无法打开 Euphony 设置：\n" + wide(error.what());
        if (test_output.empty()) MessageBoxW(nullptr, message.c_str(), L"Euphony", MB_OK | MB_ICONERROR);
        else {
            try {
                if (test_output.is_absolute()) {
                    std::filesystem::create_directories(test_output);
                    std::ofstream file(test_output / L"result.json", std::ios::binary | std::ios::trunc);
                    file << json({{"version", 1}, {"ok", false}, {"error", error.what()}}).dump(2) << '\n';
                }
            } catch (...) {}
        }
    }
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return result;
}
