#include "tray.h"
#include "common/resource.h"
#include <shellapi.h>
#include <objbase.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace euphony {
namespace {
using nlohmann::json;
constexpr UINT tray_message = WM_APP + 20, reply_message = WM_APP + 21;
constexpr UINT mode_message = WM_APP + 22;
constexpr UINT_PTR icon_retry_timer = 1;
constexpr UINT settings_id = 100, exit_id = 101, pinyin_id = 110, scheme_id = 120, page_id = 140, toggle_id = 160, layout_id = 180;
constexpr GUID tray_guid = {0x830ee227,0x261b,0x4e2b,{0x94,0x51,0xca,0x53,0x2e,0xd7,0xdc,0xe5}};
constexpr wchar_t tray_class[] = L"Euphony.Broker.Tray";
// Shell's compatibility interface is optional; newer shells use IconGuid settings.
struct ShellNotifyItem { LPWSTR executable; LPWSTR text; HICON icon; HWND window; DWORD preference; UINT id; GUID guid; };
struct __declspec(uuid("D133CE13-3537-48BA-93A7-AFCD5D2053B4")) TrayPreference : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(IUnknown*,ULONGLONG*)=0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(ULONGLONG)=0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const ShellNotifyItem*)=0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL)=0;
    virtual HRESULT STDMETHODCALLTYPE DoAction(BOOL)=0;
};
struct __declspec(uuid("FB852B2C-6BAD-4605-9551-F15F87830935")) LegacyTrayPreference : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(IUnknown*)=0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const ShellNotifyItem*)=0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL)=0;
};
void promote_compatibility(HWND window,HICON icon,REFGUID guid) {
    constexpr CLSID server={0x25dead04,0x1eac,0x4911,{0x9e,0x3a,0xad,0x0a,0x4a,0xb5,0x60,0xfd}};
    auto executable=(module_directory()/L"EuphonyBroker.exe").wstring();
    wchar_t text[]=L"Euphony · 右键快速配置，单击打开设置";
    ShellNotifyItem item{executable.data(),text,icon,window,2,1,guid};
    TrayPreference* current{};
    if (SUCCEEDED(CoCreateInstance(server,nullptr,CLSCTX_ALL,__uuidof(TrayPreference),reinterpret_cast<void**>(&current)))) {
        current->SetPreference(&item); current->Release();
    } else {
        LegacyTrayPreference* legacy{};
        if (SUCCEEDED(CoCreateInstance(server,nullptr,CLSCTX_ALL,__uuidof(LegacyTrayPreference),reinterpret_cast<void**>(&legacy)))) {
            legacy->SetPreference(&item); legacy->Release();
        }
    }
}
// Promote this icon only. Never change Explorer's global overflow preference.
void promote_icon(REFGUID guid) {
    wchar_t expected[40]{}; StringFromGUID2(guid,expected,40);
    HKEY root{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER,L"Control Panel\\NotifyIconSettings",0,KEY_READ,&root)!=ERROR_SUCCESS) return;
    for (DWORD index=0;;++index) {
        wchar_t name[256]{}; DWORD count=256;
        if (RegEnumKeyExW(root,index,name,&count,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS) break;
        HKEY item{};
        if (RegOpenKeyExW(root,name,0,KEY_QUERY_VALUE|KEY_SET_VALUE,&item)!=ERROR_SUCCESS) continue;
        wchar_t value[80]{}; DWORD bytes=sizeof(value),type{};
        if (RegQueryValueExW(item,L"IconGuid",nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes)==ERROR_SUCCESS &&
            type==REG_SZ && bytes>=sizeof(wchar_t) && bytes<=sizeof(value) && value[bytes/sizeof(wchar_t)-1]==0 && !_wcsicmp(value,expected)) {
            DWORD promoted=1; RegSetValueExW(item,L"IsPromoted",0,REG_DWORD,reinterpret_cast<BYTE*>(&promoted),sizeof(promoted));
        }
        RegCloseKey(item);
    }
    RegCloseKey(root);
}
struct Toggle { const char* key; const wchar_t* label; };
constexpr Toggle toggles[] = {{"model_enabled", L"启用 Phono 模型"}, {"prefer_unigram_single_syllable", L"无上下文单字优先词频"},
    {"chinese_punctuation", L"中文标点"}, {"full_width", L"全角字符"}, {"learning", L"记住词库选择"}, {"use_surrounding", L"读取应用上文"}};
constexpr const char* schemes[] = {"xiaohe", "ziranma", "ms", "ziguang", "abc", "zhongwenzhixing", "pinyinjiajia"};
constexpr const wchar_t* scheme_names[] = {L"小鹤双拼", L"自然码双拼", L"微软双拼", L"紫光双拼", L"智能 ABC 双拼", L"中文之星双拼", L"拼音加加双拼"};
constexpr int page_sizes[] = {5, 7, 9, 10};

HMENU make_menu(const json& config, const json& status) {
    HMENU menu = CreatePopupMenu(), modes = CreatePopupMenu(), pages = CreatePopupMenu(), layouts = CreatePopupMenu();
    if (!menu || !modes || !pages || !layouts) { if (menu) DestroyMenu(menu); if (modes) DestroyMenu(modes); if (pages) DestroyMenu(pages); if (layouts) DestroyMenu(layouts); return nullptr; }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.value("language_model_ready", false) ? L"Euphony · KenLM 已就绪" : L"Euphony · KenLM 未就绪");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(modes, MF_STRING, pinyin_id, L"全拼");
    for (UINT i = 0; i < std::size(schemes); ++i) AppendMenuW(modes, MF_STRING, scheme_id + i, scheme_names[i]);
    UINT selected = pinyin_id;
    if (config.value("input_mode", "pinyin") == "shuangpin")
        for (UINT i = 0; i < std::size(schemes); ++i) if (config.value("shuangpin_scheme", "") == schemes[i]) selected = scheme_id + i;
    CheckMenuRadioItem(modes, pinyin_id, scheme_id + static_cast<UINT>(std::size(schemes)) - 1, selected, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), L"输入方案");
    for (UINT i = 0; i < std::size(page_sizes); ++i) {
        const auto label = std::to_wstring(page_sizes[i]) + L" 个候选";
        AppendMenuW(pages, MF_STRING, page_id + i, label.c_str());
        if (config.value("page_size", 5) == page_sizes[i]) CheckMenuRadioItem(pages, page_id, page_id + 3, page_id + i, MF_BYCOMMAND);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pages), L"每页候选数");
    AppendMenuW(layouts, MF_STRING, layout_id, L"纵向");
    AppendMenuW(layouts, MF_STRING, layout_id + 1, L"横向");
    CheckMenuRadioItem(layouts, layout_id, layout_id + 1, layout_id + (config.value("candidate_layout", "vertical") == "horizontal"), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layouts), L"候选排列");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (UINT i = 0; i < std::size(toggles); ++i)
        AppendMenuW(menu, MF_STRING | (config.value(toggles[i].key, false) ? MF_CHECKED : MF_UNCHECKED), toggle_id + i, toggles[i].label);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, settings_id, L"打开设置…");
    SetMenuDefaultItem(menu, settings_id, FALSE);
    AppendMenuW(menu, MF_STRING, exit_id, L"退出后台");
    return menu;
}
json patch_for(UINT command, const json& current) {
    if (command == pinyin_id) return {{"input_mode", "pinyin"}};
    if (command >= scheme_id && command < scheme_id + std::size(schemes))
        return {{"input_mode", "shuangpin"}, {"shuangpin_scheme", schemes[command - scheme_id]}};
    if (command >= page_id && command < page_id + std::size(page_sizes)) return {{"page_size", page_sizes[command - page_id]}};
    if (command == layout_id || command == layout_id + 1) return {{"candidate_layout", command == layout_id ? "vertical" : "horizontal"}};
    if (command >= toggle_id && command < toggle_id + std::size(toggles)) {
        const auto* key = toggles[command - toggle_id].key; return {{key, !current.at(key).get<bool>()}};
    }
    return json::object();
}
}

struct BrokerTray::Impl {
    struct Job { UINT command = 0; bool menu = false; };
    struct Reply { Job job; json value; };
    std::atomic_bool& broker_stopping;
    std::atomic<HWND> window{nullptr};
    UniqueHandle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::thread ui, worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Reply> replies;
    bool closing = false, menu_requested = false, icon_added = false;
    bool shell_test = false;
    UINT add_attempts = 0;
    GUID icon_guid = tray_guid;
    UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    HICON icon = nullptr;
    bool chinese = true;
    struct MenuLabel { std::wstring text; };
    std::deque<MenuLabel> menu_labels;
    HFONT menu_font{};
    HTHEME menu_theme{};
    int menu_dpi=96;
    POINT menu_point{};
    std::filesystem::path test_output;
    int test_phase = 0;
    json original_config;

    explicit Impl(std::atomic_bool& stopping) : broker_stopping(stopping) {
        if (!ready) throw std::runtime_error("Cannot initialize Euphony tray thread");
        wchar_t path[32768]{};
        const auto length = GetEnvironmentVariableW(L"EUPHONY_TRAY_TEST", path, static_cast<DWORD>(std::size(path)));
        if (length && length < std::size(path)) test_output = path;
        shell_test = !test_output.empty() && GetEnvironmentVariableW(L"EUPHONY_TRAY_TEST_SHELL", nullptr, 0) > 1;
        if (shell_test) CoCreateGuid(&icon_guid); // Never replace the daily-use icon in an isolated Shell test.
        ui = std::thread([this] { run(); });
        if (ready) WaitForSingleObject(ready.get(), INFINITE);
    }
    ~Impl() {
        if (auto hwnd = window.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (ui.joinable()) ui.join();
    }
    void add_icon() {
        if (!test_output.empty() && !shell_test) return;
        if (icon_added) return;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window.load(); data.uID = 1; data.guidItem = icon_guid; data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
        data.hIcon = icon; data.uCallbackMessage = tray_message; wcscpy_s(data.szTip, L"Euphony · 右键快速配置，单击打开设置");
        // Exercise a delayed Shell startup in the explicit integration test.
        if (shell_test && ++add_attempts == 1) { SetTimer(window.load(), icon_retry_timer, 1000, nullptr); return; }
        icon_added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (icon_added) {
            KillTimer(window.load(), icon_retry_timer);
            data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data);
            if (test_output.empty()) { promote_compatibility(window.load(),icon,icon_guid); promote_icon(icon_guid); SetTimer(window.load(),2,1500,nullptr); }
        } else SetTimer(window.load(), icon_retry_timer, 1000, nullptr);
    }
    void remove_icon() {
        if (window.load()) KillTimer(window.load(), icon_retry_timer);
        if (icon_added) { NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window.load(); data.uID = 1; data.uFlags = NIF_GUID; data.guidItem = icon_guid; Shell_NotifyIconW(NIM_DELETE, &data); icon_added = false; }
    }
    void queue(Job job) {
        std::lock_guard lock(mutex);
        if (closing || jobs.size() >= 8) return;
        jobs.push_back(job); wake.notify_one();
    }
    void open_settings() {
        const auto file = module_directory() / L"EuphonySettings.exe";
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window.load(), L"open", file.c_str(), nullptr, file.parent_path().c_str(), SW_SHOWNORMAL));
        if (result <= 32) MessageBoxW(window.load(), L"无法打开设置。请检查 EuphonySettings.exe 和 WebView2 Runtime。", L"Euphony", MB_OK | MB_ICONERROR);
    }
    void command(UINT id) {
        if (id == settings_id) open_settings();
        else if (id == exit_id) { broker_stopping.store(true); PostMessageW(window.load(), WM_CLOSE, 0, 0); }
        else if (id) queue({id, false});
    }
    void request_menu(POINT point) {
        if (menu_requested) return;
        GetCursorPos(&point);
        menu_point = point; menu_requested = true; queue({0, true});
    }
    int menu_scale(int value) const { return MulDiv(value,menu_dpi,96); }
    void style_menu(HMENU menu) {
        for (int position=0;position<GetMenuItemCount(menu);++position) {
            wchar_t text[256]{};
            MENUITEMINFOW item{sizeof(item)}; item.fMask=MIIM_STRING|MIIM_FTYPE|MIIM_SUBMENU;
            item.dwTypeData=text; item.cch=256;
            if (!GetMenuItemInfoW(menu,position,TRUE,&item)) continue;
            if (item.hSubMenu) style_menu(item.hSubMenu);
            if (item.fType&MFT_SEPARATOR) continue;
            menu_labels.push_back({text});
            item.fMask=MIIM_FTYPE|MIIM_DATA; item.fType|=MFT_OWNERDRAW;
            item.dwItemData=reinterpret_cast<ULONG_PTR>(&menu_labels.back());
            SetMenuItemInfoW(menu,position,TRUE,&item);
        }
    }
    void begin_menu_style(HMENU menu) {
        // A hidden owner on the cursor's monitor supplies the correct menu DPI.
        SetWindowPos(window.load(),nullptr,menu_point.x,menu_point.y,1,1,SWP_NOACTIVATE|SWP_NOZORDER);
        menu_dpi=static_cast<int>(GetDpiForWindow(window.load()));
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,menu_dpi);
        metrics.lfMenuFont.lfQuality=ANTIALIASED_QUALITY;
        menu_font=CreateFontIndirectW(&metrics.lfMenuFont);
        menu_theme=OpenThemeData(window.load(),L"Menu");
        style_menu(menu);
    }
    void end_menu_style() {
        if (menu_theme) CloseThemeData(menu_theme); menu_theme=nullptr;
        if (menu_font) DeleteObject(menu_font); menu_font=nullptr; menu_labels.clear();
    }
    bool measure_menu(MEASUREITEMSTRUCT& item) {
        if (item.CtlType!=ODT_MENU || !item.itemData || !menu_font) return false;
        const auto& label=*reinterpret_cast<const MenuLabel*>(item.itemData);
        auto dc=GetDC(window.load()); auto old=SelectObject(dc,menu_font); RECT bounds{};
        DrawTextW(dc,label.text.c_str(),-1,&bounds,DT_SINGLELINE|DT_CALCRECT|DT_NOPREFIX);
        SelectObject(dc,old); ReleaseDC(window.load(),dc);
        item.itemWidth=bounds.right+menu_scale(58); item.itemHeight=std::max(static_cast<int>(bounds.bottom),menu_scale(18))+menu_scale(6);
        return true;
    }
    bool draw_menu(DRAWITEMSTRUCT& item) {
        if (item.CtlType!=ODT_MENU || !item.itemData || !menu_font) return false;
        const auto& label=*reinterpret_cast<const MenuLabel*>(item.itemData);
        const bool disabled=(item.itemState&(ODS_DISABLED|ODS_GRAYED))!=0,hot=(item.itemState&ODS_SELECTED)!=0;
        const int state=disabled?(hot?MPI_DISABLEDHOT:MPI_DISABLED):(hot?MPI_HOT:MPI_NORMAL);
        FillRect(item.hDC,&item.rcItem,GetSysColorBrush(COLOR_MENU));
        if (menu_theme) {
            DrawThemeBackground(menu_theme,item.hDC,MENU_POPUPBACKGROUND,0,&item.rcItem,nullptr);
            DrawThemeBackground(menu_theme,item.hDC,MENU_POPUPITEM,state,&item.rcItem,nullptr);
        } else if (hot) FillRect(item.hDC,&item.rcItem,GetSysColorBrush(COLOR_HIGHLIGHT));
        COLORREF color=GetSysColor(disabled?COLOR_GRAYTEXT:(hot?COLOR_HIGHLIGHTTEXT:COLOR_MENUTEXT));
        if (menu_theme) GetThemeColor(menu_theme,MENU_POPUPITEM,state,TMT_TEXTCOLOR,&color);
        auto old=SelectObject(item.hDC,menu_font); auto saved=SaveDC(item.hDC);
        SetBkMode(item.hDC,TRANSPARENT); SetTextColor(item.hDC,color);
        RECT text=item.rcItem; text.left+=menu_scale(32); text.right-=menu_scale(24);
        DrawTextW(item.hDC,label.text.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        if (item.itemState&ODS_CHECKED) {
            RECT check=item.rcItem; check.left+=menu_scale(8); check.right=check.left+menu_scale(16);
            check.top+=(check.bottom-check.top-menu_scale(16))/2; check.bottom=check.top+menu_scale(16);
            if (menu_theme) DrawThemeBackground(menu_theme,item.hDC,MENU_POPUPCHECK,disabled?MC_CHECKMARKDISABLED:MC_CHECKMARKNORMAL,&check,nullptr);
            else DrawTextW(item.hDC,L"✓",1,&check,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
        }
        RestoreDC(item.hDC,saved); SelectObject(item.hDC,old); return true;
    }
    void service() {
        IpcClient client(15000, false);
        while (true) {
            Job job;
            { std::unique_lock lock(mutex); wake.wait(lock, [&] { return closing || !jobs.empty(); }); if (closing) break; job = jobs.front(); jobs.pop_front(); }
            json value;
            try {
                // Read again when applying a command, so another settings window
                // cannot make a menu toggle overwrite unrelated saved changes.
                value = client.call({{"version", 1}, {"op", "get_config"}});
                if (job.command) {
                    const auto patch = patch_for(job.command, value.at("config"));
                    if (!patch.empty()) value = client.call({{"version", 1}, {"op", "set_config"}, {"config", patch}});
                }
            } catch (const std::exception& error) { value = {{"ok", false}, {"error", error.what()}}; }
            { std::lock_guard lock(mutex); if (closing) break; replies.push_back({job, std::move(value)}); }
            PostMessageW(window.load(), reply_message, 0, 0);
        }
    }
    void test_result(json report) {
        try {
            std::filesystem::create_directories(test_output.parent_path());
            std::ofstream file(test_output); file << report.dump(2) << '\n';
        } catch (...) {}
        PostMessageW(window.load(), WM_CLOSE, 0, 0);
    }
    void test_step(const json& value) {
        if (!value.value("ok", false)) { test_result({{"ok", false}, {"error", value.value("error", "Tray IPC failed")}}); return; }
        const auto& current = value.at("config");
        if (test_phase == 0) {
            original_config = current;
            auto menu = make_menu(current, value.at("status"));
            const bool valid = menu && icon && GetMenuItemCount(menu) == 15 && GetMenuItemCount(GetSubMenu(menu, 2)) == 8 &&
                GetMenuItemCount(GetSubMenu(menu, 3)) == 4 && GetMenuDefaultItem(menu, FALSE, 0) == settings_id &&
                GetMenuItemCount(GetSubMenu(menu, 4)) == 2 &&
                bool(GetMenuState(menu, toggle_id + 1, MF_BYCOMMAND) & MF_CHECKED) == current.at("prefer_unigram_single_syllable").get<bool>();
            if (menu) DestroyMenu(menu);
            if (!valid) { test_result({{"ok", false}, {"error", "Native tray menu or icon validation failed"}}); return; }
            ++test_phase; command(toggle_id + 1);
        } else if (test_phase == 1) {
            if (current.at("prefer_unigram_single_syllable") == original_config.at("prefer_unigram_single_syllable")) {
                test_result({{"ok", false}, {"error", "Tray command did not save the unigram option"}}); return;
            }
            ++test_phase; command(toggle_id + 1);
        } else if (test_phase == 2) {
            if (current != original_config) { test_result({{"ok", false}, {"error", "Tray unigram shortcut was not restored"}}); return; }
            ++test_phase; command(layout_id + (current.at("candidate_layout") == "vertical" ? 1 : 0));
        } else if (test_phase == 3) {
            if (current.at("candidate_layout") == original_config.at("candidate_layout")) { test_result({{"ok", false}, {"error", "Tray layout shortcut was not saved"}}); return; }
            ++test_phase; command(layout_id + (original_config.at("candidate_layout") == "horizontal" ? 1 : 0));
        } else {
            bool shell_valid = true;
            if (shell_test) {
                // Recreate only this test icon, then exercise the actual restart handler.
                remove_icon(); SendMessageW(window.load(), taskbar_created, 0, 0);
                NOTIFYICONIDENTIFIER id{sizeof(id)}; id.hWnd = window.load(); id.uID = 1; id.guidItem = icon_guid;
                RECT bounds{};
                shell_valid = icon_added && add_attempts >= 3 && SUCCEEDED(Shell_NotifyIconGetRect(&id, &bounds)) && bounds.bottom > bounds.top;
            }
            test_result({{"ok", current == original_config && shell_valid}, {"native_menu", true}, {"icon_resource", bool(icon)},
                {"shortcut_saved_via_ipc", true}, {"layout_saved_via_ipc", true}, {"config_restored", current == original_config},
                {"shell_test", shell_test}, {"shell_retry_and_recreation", shell_test && shell_valid}});
        }
    }
    void deliver() {
        std::deque<Reply> responses;
        { std::lock_guard lock(mutex); responses.swap(replies); }
        for (auto& response : responses) {
            if (!test_output.empty()) { test_step(response.value); continue; }
            if (!response.value.value("ok", false)) {
                menu_requested = false;
                MessageBoxW(window.load(), wide(response.value.value("error", "配置修改失败，请重试。")).c_str(), L"Euphony", MB_OK | MB_ICONERROR); continue;
            }
            const auto& config = response.value.at("config");
            if (icon_added) {
                const auto tip = std::wstring(L"Euphony · ") + (config.value("input_mode", "pinyin") == "pinyin" ? L"全拼" : L"双拼") +
                    (config.value("model_enabled", true) ? L" · Phono 开启" : L" · 词库模式");
                NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window.load(); data.uID = 1; data.guidItem = icon_guid; data.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_GUID;
                wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &data);
            }
            if (response.job.menu) {
                auto menu = make_menu(config, response.value.at("status"));
                if (menu) {
                    begin_menu_style(menu);
                    SetForegroundWindow(window.load());
                    const auto selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, menu_point.x, menu_point.y, window.load(), nullptr);
                    DestroyMenu(menu); end_menu_style(); PostMessageW(window.load(), WM_NULL, 0, 0); command(selected);
                }
                menu_requested = false;
            }
        }
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window.store(hwnd); }
        if (self) {
            if (message==WM_MEASUREITEM && lp && self->measure_menu(*reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) return TRUE;
            if (message==WM_DRAWITEM && lp && self->draw_menu(*reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
            if (message == WM_TIMER && wp == 2) { promote_icon(self->icon_guid); KillTimer(hwnd,2); return 0; }
            if (message == mode_message) {
                self->chinese=wp!=0;
                auto icon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(self->chinese?IDI_CHINESE:IDI_ENGLISH),IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0));
                if (icon) {
                    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd=hwnd; data.uID=1; data.guidItem=self->icon_guid;
                    data.uFlags=NIF_GUID|NIF_ICON|NIF_TIP|NIF_SHOWTIP; data.hIcon=icon;
                    wcscpy_s(data.szTip,self->chinese?L"Euphony · 中文输入":L"Euphony · English");
                    if (self->icon_added) Shell_NotifyIconW(NIM_MODIFY,&data);
                    if (self->icon) DestroyIcon(self->icon); self->icon=icon;
                }
                return 0;
            }
            if (message == self->taskbar_created) { self->icon_added = false; self->add_icon(); return 0; }
            if (message == WM_TIMER && wp == icon_retry_timer) {
                self->add_icon();
                if (self->shell_test && self->icon_added && self->test_phase == 0) self->queue({});
                return 0;
            }
            if (message == tray_message) {
                const auto notification = LOWORD(lp);
                if (notification == WM_CONTEXTMENU) self->request_menu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                else if (notification == WM_LBUTTONDBLCLK || notification == NIN_SELECT || notification == NIN_KEYSELECT) self->open_settings();
                return 0;
            }
            if (message == reply_message) { self->deliver(); return 0; }
            if (message == WM_CLOSE) { self->remove_icon(); DestroyWindow(hwnd); return 0; }
            if (message == WM_DESTROY) { { std::lock_guard lock(self->mutex); self->closing = true; self->jobs.clear(); } self->window.store(nullptr); self->wake.notify_one(); PostQuitMessage(0); return 0; }
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    void run() {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = procedure; type.lpszClassName = tray_class;
        icon = static_cast<HICON>(LoadImageW(type.hInstance, MAKEINTRESOURCEW(IDI_CHINESE), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR)); type.hIcon = icon;
        if (RegisterClassW(&type)) CreateWindowExW(WS_EX_TOOLWINDOW, tray_class, L"Euphony", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, this);
        if (ready) SetEvent(ready.get());
        if (!window.load()) { if (icon) { DestroyIcon(icon); icon=nullptr; } if (SUCCEEDED(com)) CoUninitialize(); return; }
        // A manually elevated broker still needs the user shell's restart event.
        ChangeWindowMessageFilterEx(window.load(), taskbar_created, MSGFLT_ALLOW, nullptr);
        add_icon(); worker = std::thread([this] { service(); });
        if (!test_output.empty() && !shell_test) queue({});
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (worker.joinable()) worker.join();
        window.store(nullptr); UnregisterClassW(tray_class, type.hInstance);
        if (icon) { DestroyIcon(icon); icon = nullptr; }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};
BrokerTray::BrokerTray(std::atomic_bool& stopping) {
    // Ordinary automated tests keep the notification area untouched. The explicit
    // native-menu test creates a hidden owner; --visible also tests a real icon.
    if (GetEnvironmentVariableW(L"EUPHONY_INSTANCE", nullptr, 0) > 1 && GetEnvironmentVariableW(L"EUPHONY_TRAY_TEST", nullptr, 0) <= 1) return;
    impl_ = std::make_unique<Impl>(stopping);
}
BrokerTray::~BrokerTray() = default;
void BrokerTray::set_mode(bool chinese) { if (impl_) if (auto window=impl_->window.load()) PostMessageW(window,mode_message,chinese,0); }
}
