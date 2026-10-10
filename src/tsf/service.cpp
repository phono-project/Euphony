#include "guids.h"
#include "key_policy.h"
#include "display_attribute.h"
#include "common/ipc.h"
#include "common/win_util.h"
#include "common/resource.h"
#include <msctf.h>
#include <ctfutb.h>
#include <olectl.h>
#include <inputscope.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace euphony::tsf {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace {
constexpr UINT kReply = WM_APP + 91;
constexpr UINT kReplay = WM_APP + 92;
constexpr LPARAM kReplayMark = static_cast<LPARAM>(1u<<25);
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT_PTR kConfigTimer = 2;
constexpr wchar_t kWindowClass[] = L"Euphony.Tsf.Dispatch.v1";
constexpr wchar_t kPopupClass[] = L"Euphony.Tsf.Candidates.v1";
std::mutex class_mutex;
UINT class_users = 0;

class Edit final : public ITfEditSession {
    LONG refs_ = 1;
    std::function<HRESULT(TfEditCookie)> action_;
    ComPtr<IUnknown> owner_;
public:
    Edit(IUnknown* owner, std::function<HRESULT(TfEditCookie)> action) : action_(std::move(action)), owner_(owner) { InterlockedIncrement(&objects); }
    ~Edit() { InterlockedDecrement(&objects); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
        *out = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        try { return action_(cookie); } catch (...) { return E_FAIL; }
    }
};

struct Work {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Json> requests;
    HWND window{};
    bool stop = false;
    HMODULE pin{};
};
// No COM pointers cross this boundary. The native thread owns a module reference
// until it exits; deactivation only detaches its message destination.
DWORD WINAPI work_thread(void* argument) {
    auto* handle = static_cast<std::shared_ptr<Work>*>(argument);
    std::shared_ptr<Work> state = std::move(*handle); delete handle;
    IpcClient client;
    bool launched = false;
    std::vector<std::string> sessions;
    for (;;) {
        Json request;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock, [&] { return state->stop || !state->requests.empty(); });
            if (state->stop) break;
            request = std::move(state->requests.front()); state->requests.pop_front();
        }
        const auto session=request.value("session",std::string{});
        if (!session.empty() && std::find(sessions.begin(),sessions.end(),session)==sessions.end()) sessions.push_back(session);
        Json response;
        try {
            if (!launched) { ensure_broker(); launched = true; }
            response = client.call(request);
        } catch (const std::exception& e) {
            response = { {"session", request.value("session", std::string{})}, {"seq", request.value("seq", std::uint64_t{})},
                {"generation", 0}, {"error", e.what()}, {"_failed_key", request.value("key", std::string{})} };
            launched = false;
        }
        response["_op"]=request.value("op",std::string{});
        response["_key"]=request.value("key",std::string{});
        response["_vk"]=request.value("virtual_key",std::uint64_t{});
        response["_flags"]=request.value("key_flags",std::uint64_t{});
        response["_shift"]=request.value("shift",false);
        if (request.value("op",std::string{})=="close" && !response.contains("_failed_key")) sessions.erase(std::remove(sessions.begin(),sessions.end(),session),sessions.end());
        auto result = std::make_unique<Json>(std::move(response));
        {
            std::lock_guard lock(state->mutex);
            if (!state->stop && state->window && PostMessageW(state->window, kReply, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        }
    }
    // Finishing a blocked call and closing its broker state stays off the STA.
    // Deactivation has already detached the destination window and cancelled
    // queued keys, so no response can re-enter a document during cleanup.
    for (const auto& session:sessions) {
        try { client.call({{"version",1},{"session",session},{"seq",std::numeric_limits<std::uint64_t>::max()},{"op","close"}}); } catch (...) {}
    }
    std::vector<std::string>().swap(sessions);
    const auto pin = state->pin; state.reset();
    InterlockedDecrement(&objects);
    FreeLibraryAndExitThread(pin, 0);
}
std::shared_ptr<Work> start_worker(HWND window) {
    auto state = std::make_shared<Work>(); state->window = window;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&work_thread), &state->pin)) return {};
    auto* argument = new(std::nothrow) std::shared_ptr<Work>(state);
    if (!argument) { FreeLibrary(state->pin); return {}; }
    InterlockedIncrement(&objects);
    auto thread = CreateThread(nullptr, 0, work_thread, argument, 0, nullptr);
    if (!thread) { delete argument; InterlockedDecrement(&objects); FreeLibrary(state->pin); return {}; }
    CloseHandle(thread); return state;
}
std::string key_name(WPARAM vk, LPARAM flags) {
    switch (vk) {
    case VK_BACK: return "Backspace"; case VK_DELETE: return "Delete";
    case VK_LEFT: return "Left"; case VK_RIGHT: return "Right"; case VK_HOME: return "Home"; case VK_END: return "End";
    case VK_UP: return "Up"; case VK_DOWN: return "Down"; case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter"; case VK_ESCAPE: return "Escape"; case VK_TAB: return "Tab";
    case VK_PRIOR: return "PageUp"; case VK_NEXT: return "PageDown";
    }
    if (vk >= 'A' && vk <= 'Z') {
        const bool uppercase=((GetKeyState(VK_SHIFT)&0x8000)!=0) != ((GetKeyState(VK_CAPITAL)&1)!=0);
        return std::string(1,static_cast<char>((uppercase?'A':'a')+vk-'A'));
    }
    BYTE state[256]{}; GetKeyboardState(state);
    wchar_t buffer[4]{};
    // Flag 4 leaves the dead-key state unchanged (Windows 10+). Pinyin letters
    // use virtual keys above; only ASCII punctuation reaches this conversion.
    auto count = ToUnicodeEx(static_cast<UINT>(vk), static_cast<UINT>((flags >> 16) & 0xff), state, buffer, 4, 4, GetKeyboardLayout(0));
    return count == 1 && buffer[0] >= 32 && buffer[0] < 127 ? std::string(1, static_cast<char>(buffer[0])) : std::string{};
}
bool compartment(IUnknown* owner, REFGUID id) {
    ComPtr<ITfCompartmentMgr> mgr; ComPtr<ITfCompartment> field;
    if (!owner || FAILED(owner->QueryInterface(IID_PPV_ARGS(&mgr))) || FAILED(mgr->GetCompartment(id, &field))) return false;
    VARIANT value{}; VariantInit(&value);
    const bool result = SUCCEEDED(field->GetValue(&value)) && value.vt == VT_I4 && value.lVal != 0;
    VariantClear(&value); return result;
}
struct Candidate { std::wstring text; bool model{}, continues{}; std::vector<std::pair<std::wstring, bool>> pieces; };
struct Replay { ComPtr<ITfContext> context; std::uint64_t epoch{}; WPARAM vk{}; LPARAM flags{}; };
class Service;
class LanguageButton final : public ITfLangBarItemButton, public ITfSource {
    LONG refs_ = 1;
    Service* owner_;
    ComPtr<ITfLangBarItemSink> sink_;
public:
    explicit LanguageButton(Service* owner) : owner_(owner) { InterlockedIncrement(&objects); }
    ~LanguageButton() { InterlockedDecrement(&objects); }
    void detach() { owner_ = nullptr; sink_.Reset(); }
    void update() { if (sink_) sink_->OnUpdate(TF_LBI_TEXT | TF_LBI_ICON | TF_LBI_TOOLTIP); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO*) override;
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD* status) override { if (!status) return E_POINTER; *status=0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Show(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR*) override;
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick, POINT, const RECT*) override;
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu*) override;
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT) override;
    HRESULT STDMETHODCALLTYPE GetIcon(HICON*) override;
    HRESULT STDMETHODCALLTYPE GetText(BSTR*) override;
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID, IUnknown*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD) override;
};

class Service final : public ITfTextInputProcessorEx, public ITfKeyEventSink,
    public ITfCompositionSink, public ITfThreadMgrEventSink, public ITfThreadFocusSink,
    public ITfCandidateListUIElementBehavior, public ITfDisplayAttributeProvider,
    public ITfTextEditSink, public ITfTextLayoutSink {
    LONG refs_ = 1;
    ComPtr<ITfThreadMgr> mgr_;
    ComPtr<ITfContext> context_;
    ComPtr<ITfComposition> composition_;
    ComPtr<ITfUIElementMgr> ui_;
    ComPtr<ITfLangBarItemMgr> langbar_;
    ComPtr<LanguageButton> button_;
    TfClientId client_ = TF_CLIENTID_NULL;
    TfGuidAtom attribute_ = TF_INVALID_GUIDATOM;
    DWORD events_cookie_ = TF_INVALID_COOKIE, focus_cookie_ = TF_INVALID_COOKIE, ui_id_ = TF_INVALID_COOKIE;
    DWORD edit_cookie_ = TF_INVALID_COOKIE, layout_cookie_ = TF_INVALID_COOKIE;
    DWORD activation_{};
    HWND dispatch_{}, popup_{}, mode_popup_{};
    COLORREF model_color_ = RGB(223,242,229);
    HFONT font_{};
    std::shared_ptr<Work> worker_;
    std::string session_;
    std::uint64_t epoch_ = 0, seq_ = 0, key_seq_ = 0, applied_seq_ = 0, generation_ = 0, candidate_generation_ = 0;
    std::uint64_t rendered_seq_ = 0;
    std::uint64_t config_epoch_ = 0;
    bool use_surrounding_ = false;
    bool full_width_ = false, chinese_punctuation_ = true;
    UINT history_characters_ = 128;
    bool chinese_ = true, active_ = false, focused_ = true, shift_used_ = false;
    bool ui_allowed_ = true, pending_ = false, poll_queued_ = false, failed_ = false;
    bool config_queued_ = false, classes_owned_ = false;
    std::wstring preedit_;
    std::vector<Candidate> candidates_;
    std::vector<UINT> host_pages_;
    UINT selection_ = 0, page_ = 0, page_size_ = 5;
    RECT caret_{};
    bool caret_valid_ = false, horizontal_ = false, updating_ui_ = false;
    std::vector<RECT> candidate_cells_;
    int row_ = 32, pad_ = 12;
    std::deque<std::pair<std::uint64_t, std::string>> outstanding_;
    std::wstring recovery_;

    IUnknown* identity() { return static_cast<ITfTextInputProcessorEx*>(this); }
    HRESULT edit(ITfContext* context, DWORD flags, std::function<HRESULT(TfEditCookie)> action) {
        if (!context) return E_FAIL;
        auto* request = new(std::nothrow) Edit(identity(), std::move(action));
        if (!request) return E_OUTOFMEMORY;
        HRESULT result = E_FAIL;
        auto hr = context->RequestEditSession(client_, request, flags, &result); request->Release();
        return FAILED(hr) ? hr : result;
    }
    Json request(std::string op) {
        return { {"version",1}, {"session",session_}, {"seq",++seq_}, {"op",std::move(op)},
            {"key",""}, {"shift",false}, {"ctrl",false}, {"alt",false}, {"surrounding",""},
            {"sensitive",false}, {"context_valid",false} };
    }
    bool enqueue(Json value) {
        if (!worker_) return false;
        std::lock_guard lock(worker_->mutex);
        if (worker_->stop || worker_->requests.size() >= 256) return false;
        worker_->requests.push_back(std::move(value)); worker_->wake.notify_one(); return true;
    }
    void hide() {
        if (popup_) ShowWindow(popup_, SW_HIDE);
        caret_valid_=false; candidate_cells_.clear();
        if (ui_ && ui_id_ != TF_INVALID_COOKIE) { const auto id=std::exchange(ui_id_,TF_INVALID_COOKIE); ui_->EndUIElement(id); }
        candidates_.clear(); host_pages_.clear(); pending_=false;
        if (dispatch_) KillTimer(dispatch_, kPollTimer);
    }
    void rotate() {
        if (!session_.empty()) enqueue(request("close"));
        GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id,text,40);
        session_=utf8(text); ++epoch_; seq_=key_seq_=applied_seq_=generation_=candidate_generation_=rendered_seq_=0;
        active_=false; failed_=false; poll_queued_=false; outstanding_.clear(); recovery_.clear();
        caret_valid_=false; candidate_cells_.clear();
    }
    HRESULT end(TfEditCookie cookie, bool cancel) {
        if (!composition_) return S_OK;
        auto composition=composition_; composition_.Reset();
        ComPtr<ITfRange> range;
        if (cancel && SUCCEEDED(composition->GetRange(&range))) range->SetText(cookie,0,L"",0);
        return composition->EndComposition(cookie);
    }
    void cancel() {
        hide(); rotate(); preedit_.clear();
        if (composition_ && context_) {
            auto context=context_; const auto epoch=epoch_;
            edit(context.Get(), TF_ES_ASYNC|TF_ES_READWRITE, [this,context,epoch](TfEditCookie cookie) {
                return context_.Get()==context.Get() && epoch_==epoch ? end(cookie,true) : S_OK;
            });
        }
    }
    void focus_context(ITfContext* next) {
        if (context_.Get()==next) return;
        auto old=context_; auto composition=composition_;
        ComPtr<ITfSource> source;
        if (old && SUCCEEDED(old.As(&source))) {
            if (edit_cookie_!=TF_INVALID_COOKIE) source->UnadviseSink(edit_cookie_);
            if (layout_cookie_!=TF_INVALID_COOKIE) source->UnadviseSink(layout_cookie_);
        }
        edit_cookie_=layout_cookie_=TF_INVALID_COOKIE; source.Reset();
        composition_.Reset(); hide(); rotate(); preedit_.clear(); context_=next;
        if (context_ && SUCCEEDED(context_.As(&source))) {
            source->AdviseSink(IID_ITfTextEditSink,static_cast<ITfTextEditSink*>(this),&edit_cookie_);
            source->AdviseSink(IID_ITfTextLayoutSink,static_cast<ITfTextLayoutSink*>(this),&layout_cookie_);
        }
        if (old && composition) edit(old.Get(),TF_ES_ASYNC|TF_ES_READWRITE,[old,composition](TfEditCookie cookie) {
            ComPtr<ITfRange> range;
            if (SUCCEEDED(composition->GetRange(&range))) range->SetText(cookie,0,L"",0);
            return composition->EndComposition(cookie);
        });
    }
    bool sensitive(ITfContext* context,TfEditCookie cookie) {
        TF_STATUS status{};
        if (!context || FAILED(context->GetStatus(&status)) || (status.dwDynamicFlags&TF_SD_READONLY) ||
            compartment(context,GUID_COMPARTMENT_KEYBOARD_DISABLED) || compartment(context,GUID_COMPARTMENT_EMPTYCONTEXT) ||
            (activation_&TF_TMAE_SECUREMODE)) return true;
        TF_SELECTION selected{}; ULONG selected_count{};
        if (FAILED(context->GetSelection(cookie,TF_DEFAULT_SELECTION,1,&selected,&selected_count)) || !selected_count) return true;
        ComPtr<ITfRange> range; range.Attach(selected.range);
        ComPtr<ITfReadOnlyProperty> prop;
        if (SUCCEEDED(context->GetAppProperty(kInputScopeProperty,&prop))) {
            VARIANT result{}; VariantInit(&result);
            bool private_scope=false;
            if (SUCCEEDED(prop->GetValue(cookie,range.Get(),&result)) && result.vt==VT_UNKNOWN && result.punkVal) {
                ComPtr<ITfInputScope> scope;
                if (SUCCEEDED(result.punkVal->QueryInterface(IID_PPV_ARGS(&scope)))) {
                    InputScope* scopes{}; UINT scope_count{};
                    if (SUCCEEDED(scope->GetInputScopes(&scopes,&scope_count))) {
                        for (UINT i=0;i<scope_count;++i) private_scope |= scopes[i]==IS_PASSWORD || scopes[i]==IS_PRIVATE ||
                            scopes[i]==IS_NUMERIC_PASSWORD || scopes[i]==IS_NUMERIC_PIN || scopes[i]==IS_ALPHANUMERIC_PIN || scopes[i]==IS_ALPHANUMERIC_PIN_SET;
                        CoTaskMemFree(scopes);
                    }
                }
            }
            VariantClear(&result); if (private_scope) return true;
        }
        ComPtr<ITfContextView> view;
        if (SUCCEEDED(context->GetActiveView(&view))) { HWND window{}; view->GetWnd(&window); if (window && (GetWindowLongPtrW(window,GWL_STYLE)&ES_PASSWORD)) return true; }
        return false;
    }
    bool locate_caret(ITfContextView* view, TfEditCookie cookie, ITfRange* range) {
        RECT next{}; BOOL clipped{};
        // GetTextExt may fail during relayout, or succeed with an empty rectangle.
        // Neither result may replace the last usable position with the origin.
        if (!view || FAILED(view->GetTextExt(cookie,range,&next,&clipped)) || next.bottom<=next.top || next.right<next.left) return false;
        caret_=next; caret_valid_=true; return true;
    }
    bool snapshot(ITfContext* context, Json& value) {
        TF_STATUS status{};
        if (!context || FAILED(context->GetStatus(&status)) || (status.dwDynamicFlags&TF_SD_READONLY) ||
            compartment(context,GUID_COMPARTMENT_KEYBOARD_DISABLED) || compartment(context,GUID_COMPARTMENT_EMPTYCONTEXT)) return false;
        bool permitted=false;
        auto hr=edit(context,TF_ES_SYNC|TF_ES_READ,[&](TfEditCookie cookie) {
            value["sensitive"]=sensitive(context,cookie);
            if (value["sensitive"].get<bool>()) return S_OK;
            TF_SELECTION selected{}; ULONG count{};
            if (FAILED(context->GetSelection(cookie,TF_DEFAULT_SELECTION,1,&selected,&count)) || !count) return E_FAIL;
            ComPtr<ITfRange> range; range.Attach(selected.range);
            ComPtr<ITfContextView> view;
            if (SUCCEEDED(context->GetActiveView(&view))) locate_caret(view.Get(),cookie,range.Get());
            permitted=true;
            if (!use_surrounding_ || !history_characters_ || key_seq_>rendered_seq_) return S_OK;
            // Read only the left prefix before our composition, never the full document.
            if (composition_ && FAILED(composition_->GetRange(&range))) return E_FAIL;
            range->Collapse(cookie,TF_ANCHOR_START);
            ComPtr<ITfRange> prefix; range->Clone(&prefix);
            const auto units=history_characters_*2+1;
            LONG moved{}; prefix->ShiftStart(cookie,-static_cast<LONG>(units),&moved,nullptr);
            std::vector<wchar_t> buffer(units+1); ULONG length{};
            if (SUCCEEDED(prefix->GetText(cookie,0,buffer.data(),units,&length))) {
                std::wstring text(buffer.data(),length);
                if (!text.empty() && text.front()>=0xdc00 && text.front()<=0xdfff) text.erase(text.begin());
                if (!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff) text.pop_back();
                std::size_t start=text.size(); UINT characters=0;
                while (start && characters<history_characters_) { --start; if (text[start]>=0xdc00 && text[start]<=0xdfff && start && text[start-1]>=0xd800 && text[start-1]<=0xdbff) --start; ++characters; }
                text.erase(0,start);
                value["surrounding"]=utf8(text); value["context_valid"]=true;
            }
            return S_OK;
        });
        return SUCCEEDED(hr) && permitted;
    }
    HRESULT render(TfEditCookie cookie, const Json& value) {
        if (sensitive(context_.Get(),cookie)) { end(cookie,true); hide(); rotate(); preedit_.clear(); return S_OK; }
        const auto commit=wide(value.value("commit",std::string{}));
        const auto preedit=wide(value.value("preedit",std::string{}));
        ComPtr<ITfRange> range;
        if (composition_) composition_->GetRange(&range);
        if (!range) {
            TF_SELECTION selected{}; ULONG count{};
            auto hr=context_->GetSelection(cookie,TF_DEFAULT_SELECTION,1,&selected,&count);
            if (FAILED(hr) || !count) return E_FAIL;
            range.Attach(selected.range);
        }
        if (!commit.empty()) {
            auto hr=range->SetText(cookie,0,commit.data(),static_cast<LONG>(commit.size()));
            if (FAILED(hr)) return hr;
            end(cookie,false); range->Collapse(cookie,TF_ANCHOR_END);
            const auto back=std::clamp(value.value("commit_cursor_back",0),0,static_cast<int>(commit.size()));
            if (back) {
                LONG moved{}; const auto cursor=utf16_boundary(commit,commit.size()-back);
                range->ShiftStart(cookie,-static_cast<LONG>(commit.size()-cursor),&moved,nullptr); range->Collapse(cookie,TF_ANCHOR_START);
            }
            TF_SELECTION selected{range.Get(),{TF_AE_NONE,FALSE}}; context_->SetSelection(cookie,1,&selected);
        }
        if (!preedit.empty()) {
            if (!composition_) {
                ComPtr<ITfContextComposition> manager;
                auto hr=context_.As(&manager);
                if (FAILED(hr)) return hr;
                hr=manager->StartComposition(cookie,range.Get(),this,&composition_);
                if (FAILED(hr) || !composition_) return E_FAIL;
            }
            composition_->GetRange(&range);
            auto hr=range->SetText(cookie,0,preedit.data(),static_cast<LONG>(preedit.size()));
            if (FAILED(hr)) return hr;
            ComPtr<ITfProperty> property;
            if (attribute_!=TF_INVALID_GUIDATOM && SUCCEEDED(context_->GetProperty(GUID_PROP_ATTRIBUTE,&property))) {
                VARIANT attribute{}; attribute.vt=VT_I4; attribute.lVal=attribute_; property->SetValue(cookie,range.Get(),&attribute);
            }
            ComPtr<ITfRange> cursor; range->Clone(&cursor); cursor->Collapse(cookie,TF_ANCHOR_START);
            LONG moved{};
            auto offset=utf16_boundary(preedit,static_cast<std::size_t>(std::max(0,value.value("cursor",static_cast<int>(preedit.size())))));
            cursor->ShiftStart(cookie,static_cast<LONG>(offset),&moved,nullptr); cursor->Collapse(cookie,TF_ANCHOR_START);
            TF_SELECTION selected{cursor.Get(),{TF_AE_NONE,FALSE}}; context_->SetSelection(cookie,1,&selected);
            ComPtr<ITfContextView> view;
            if (SUCCEEDED(context_->GetActiveView(&view))) locate_caret(view.Get(),cookie,cursor.Get());
        } else if (commit.empty()) end(cookie,true);
        if (!value.value("eaten",true) && value.value("_op",std::string{})=="key" && value.value("error",std::string{}).empty()) {
            const auto key=value.value("_key",std::string{});
            TF_SELECTION selected{}; ULONG count{};
            if (SUCCEEDED(context_->GetSelection(cookie,TF_DEFAULT_SELECTION,1,&selected,&count)) && count) {
                ComPtr<ITfRange> application; application.Attach(selected.range);
                if (key.size()==1 || key=="Space") {
                    auto text=wide(key=="Space"?" ":key);
                    application->SetText(cookie,0,text.data(),static_cast<LONG>(text.size())); application->Collapse(cookie,TF_ANCHOR_END);
                    TF_SELECTION position{application.Get(),{TF_AE_NONE,FALSE}}; context_->SetSelection(cookie,1,&position);
                } else if (key=="Backspace" || key=="Delete") {
                    BOOL empty{}; application->IsEmpty(cookie,&empty);
                    if (empty) {
                        LONG moved{}; wchar_t unit{}; ULONG length{};
                        if (key=="Backspace") { application->ShiftStart(cookie,-1,&moved,nullptr); application->GetText(cookie,0,&unit,1,&length); if (length && unit>=0xdc00 && unit<=0xdfff) application->ShiftStart(cookie,-1,&moved,nullptr); }
                        else { application->ShiftEnd(cookie,1,&moved,nullptr); application->GetText(cookie,0,&unit,1,&length); if (length && unit>=0xd800 && unit<=0xdbff) application->ShiftEnd(cookie,1,&moved,nullptr); }
                    }
                    application->SetText(cookie,0,L"",0); application->Collapse(cookie,TF_ANCHOR_START);
                    TF_SELECTION position{application.Get(),{TF_AE_NONE,FALSE}}; context_->SetSelection(cookie,1,&position);
                } else if (key=="Left" || key=="Right" || key=="Home" || key=="End") {
                    const bool shift=value.value("_shift",false);
                    BOOL empty{}; application->IsEmpty(cookie,&empty);
                    ComPtr<ITfRange> fixed,target;
                    application->Clone(&fixed); application->Clone(&target);
                    const bool start_active=selected.style.ase==TF_AE_START;
                    fixed->Collapse(cookie,start_active?TF_ANCHOR_END:TF_ANCHOR_START);
                    target->Collapse(cookie,start_active?TF_ANCHOR_START:TF_ANCHOR_END);
                    if (!shift && !empty && (key=="Left" || key=="Right")) { application->Clone(&target); target->Collapse(cookie,key=="Left"?TF_ANCHOR_START:TF_ANCHOR_END); }
                    else if (key=="Left" || key=="Right") {
                        ComPtr<ITfRange> probe; target->Clone(&probe); LONG moved{}; wchar_t unit{}; ULONG length{};
                        if (key=="Left") { probe->ShiftStart(cookie,-1,&moved,nullptr); probe->GetText(cookie,0,&unit,1,&length); target->ShiftStart(cookie,length && unit>=0xdc00 && unit<=0xdfff?-2:-1,&moved,nullptr); target->Collapse(cookie,TF_ANCHOR_START); }
                        else { probe->ShiftEnd(cookie,1,&moved,nullptr); probe->GetText(cookie,0,&unit,1,&length); target->ShiftEnd(cookie,length && unit>=0xd800 && unit<=0xdbff?2:1,&moved,nullptr); target->Collapse(cookie,TF_ANCHOR_END); }
                    } else {
                        ComPtr<ITfRange> probe; target->Clone(&probe); LONG moved{};
                        constexpr ULONG limit=32768; std::vector<wchar_t> text(limit); ULONG length{};
                        if (key=="Home") {
                            probe->ShiftStart(cookie,-static_cast<LONG>(limit),&moved,nullptr); probe->GetText(cookie,0,text.data(),limit,&length);
                            std::wstring_view prefix(text.data(),length); const auto newline=prefix.rfind(L'\n');
                            const auto amount=newline==std::wstring_view::npos?length:length-newline-1;
                            target->ShiftStart(cookie,-static_cast<LONG>(amount),&moved,nullptr); target->Collapse(cookie,TF_ANCHOR_START);
                        } else {
                            probe->ShiftEnd(cookie,static_cast<LONG>(limit),&moved,nullptr); probe->GetText(cookie,0,text.data(),limit,&length);
                            std::wstring_view suffix(text.data(),length); const auto newline=suffix.find_first_of(L"\r\n");
                            const auto amount=newline==std::wstring_view::npos?length:newline;
                            target->ShiftEnd(cookie,static_cast<LONG>(amount),&moved,nullptr); target->Collapse(cookie,TF_ANCHOR_END);
                        }
                    }
                    TfActiveSelEnd direction=TF_AE_NONE;
                    if (shift) {
                        LONG comparison{}; target->CompareStart(cookie,fixed.Get(),TF_ANCHOR_START,&comparison);
                        if (comparison<0) { fixed->ShiftStartToRange(cookie,target.Get(),TF_ANCHOR_START); direction=TF_AE_START; }
                        else { fixed->ShiftEndToRange(cookie,target.Get(),TF_ANCHOR_END); direction=TF_AE_END; }
                        target=fixed;
                    }
                    TF_SELECTION position{target.Get(),{direction,FALSE}}; context_->SetSelection(cookie,1,&position);
                } else if (value.value("_vk",std::uint64_t{}) && key!="Escape") {
                    auto replay=std::make_unique<Replay>(Replay{context_,epoch_,static_cast<WPARAM>(value.value("_vk",std::uint64_t{})),static_cast<LPARAM>(value.value("_flags",std::uint64_t{}))});
                    if (PostMessageW(dispatch_,kReplay,0,reinterpret_cast<LPARAM>(replay.get()))) replay.release();
                }
            }
        }
        preedit_=preedit; candidates_.clear();
        const auto list_generation=value.value("candidate_generation",value.value("generation",std::uint64_t{}));
        if (candidate_generation_!=list_generation) host_pages_.clear();
        candidate_generation_=list_generation;
        if (value.contains("candidates") && value["candidates"].is_array()) for (const auto& item:value["candidates"]) {
            Candidate candidate{wide(item.value("text",std::string{})),item.value("model",false),item.value("continues",false),{}};
            if (item.contains("pieces")) for (const auto& piece:item["pieces"]) candidate.pieces.emplace_back(wide(piece.value("text",std::string{})),piece.value("deleted",false));
            candidates_.push_back(std::move(candidate));
        }
        selection_=std::min(static_cast<UINT>(std::max(0,value.value("selection",0))),candidates_.empty()?0u:static_cast<UINT>(candidates_.size()-1));
        page_size_=static_cast<UINT>(std::clamp(value.value("page_size",5),1,10));
        horizontal_=value.value("candidate_layout","vertical")=="horizontal";
        const auto color=value.value("model_candidate_color",std::string{"#DFF2E5"});
        if (color.size()==7 && color.front()=='#' && color.find_first_not_of("0123456789abcdefABCDEF",1)==std::string::npos) {
            const auto rgb=std::stoul(color.substr(1),nullptr,16);
            model_color_=RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);
        }
        page_=static_cast<UINT>(std::max(0,value.value("page",0)));
        pending_=value.value("pending",false);
        update_ui(); return S_OK;
    }
    void receive(Json value) {
        if (value.value("_op",std::string{})=="set_mode") return;
        if (value.value("_op",std::string{})=="get_config") {
            config_queued_=false;
            if (value.contains("config")) {
                const auto& config=value["config"];
                use_surrounding_=config.value("use_surrounding",false);
                full_width_=config.value("full_width",false); chinese_punctuation_=config.value("chinese_punctuation",true);
                history_characters_=static_cast<UINT>(std::clamp(config.value("history_characters",128),0,8192));
            }
            return;
        }
        poll_queued_=false;
        if (value.value("session",std::string{})!=session_) return;
        use_surrounding_=value.value("use_surrounding",use_surrounding_);
        full_width_=value.value("full_width",full_width_); chinese_punctuation_=value.value("chinese_punctuation",chinese_punctuation_);
        history_characters_=static_cast<UINT>(std::clamp(value.value("history_characters",static_cast<int>(history_characters_)),0,8192));
        const auto config_epoch=value.value("config_epoch",config_epoch_);
        if (config_epoch!=config_epoch_) { config_epoch_=config_epoch; generation_=candidate_generation_=0; }
        auto seq=value.value("seq",std::uint64_t{}), generation=value.value("generation",std::uint64_t{});
        if (!value.value("error",std::string{}).empty() && value.contains("_failed_key")) {
            // Recover the raw, locally buffered input if transport is unavailable.
            // Future keys pass through until focus changes, so an unavailable broker
            // cannot trap the user's keyboard in an application.
            auto raw=recovery_; if (raw.empty()) raw=preedit_;
            hide(); rotate(); failed_=true;
            auto context=context_; auto epoch=epoch_;
            if (context) edit(context.Get(),TF_ES_ASYNC|TF_ES_READWRITE,[this,context,epoch,raw](TfEditCookie cookie) {
                return context_.Get()==context.Get() && epoch_==epoch ? render(cookie,{{"commit",utf8(raw)},{"preedit",""}}) : S_OK;
            });
            return;
        }
        if (!accepts_response(session_,value.value("session",std::string{}),applied_seq_,seq,generation_,generation)) return;
        applied_seq_=seq; generation_=generation;
        while (!outstanding_.empty() && outstanding_.front().first<=seq) outstanding_.pop_front();
        recovery_=wide(value.value("preedit",std::string{}));
        for (const auto& key:outstanding_) if (key.second.size()==1) recovery_+=wide(key.second);
        if (seq>=key_seq_) active_=!value.value("preedit",std::string{}).empty();
        auto context=context_; auto epoch=epoch_;
        if (context && focused_) edit(context.Get(),TF_ES_ASYNC|TF_ES_READWRITE,[this,context,epoch,value=std::move(value)](TfEditCookie cookie) {
            if (context_.Get()!=context.Get() || epoch_!=epoch || !focused_) return S_OK;
            auto result=render(cookie,value); if (epoch_==epoch) rendered_seq_=std::max(rendered_seq_,value.value("seq",std::uint64_t{})); return result;
        });
    }
    void update_ui() {
        if (updating_ui_) return;
        updating_ui_=true;
        struct Reset { bool& flag; ~Reset() { flag=false; } } reset{updating_ui_};
        if (preedit_.empty() || !focused_) { hide(); return; }
        if (ui_ && ui_id_==TF_INVALID_COOKIE) {
            BOOL show=TRUE;
            if (SUCCEEDED(ui_->BeginUIElement(this,&show,&ui_id_))) ui_allowed_=show!=FALSE;
        } else if (ui_ && ui_id_!=TF_INVALID_COOKIE) ui_->UpdateUIElement(ui_id_);
        if (ui_allowed_ && popup_ && caret_valid_) {
            MONITORINFO monitor{sizeof(monitor)};
            if (!GetMonitorInfoW(MonitorFromRect(&caret_,MONITOR_DEFAULTTONEAREST),&monitor)) return;
            const auto max_width=static_cast<int>(monitor.rcWork.right-monitor.rcWork.left);
            HDC dc=GetDC(popup_); auto old=SelectObject(dc,font_);
            SIZE extent{}; GetTextExtentPoint32W(dc,preedit_.data(),static_cast<int>(preedit_.size()),&extent);
            int width=std::min(max_width,std::max(220,static_cast<int>(extent.cx)+pad_*2+28));
            const auto begin=std::min<std::size_t>(page_*page_size_,candidates_.size());
            const auto limit=std::min<std::size_t>(begin+page_size_,candidates_.size());
            std::vector<int> cell_widths;
            for (auto i=begin;i<limit;++i) {
                const auto label=std::to_wstring((i-begin+1)%10)+L"  "+candidates_[i].text;
                GetTextExtentPoint32W(dc,label.data(),static_cast<int>(label.size()),&extent);
                const auto cell_width=std::min(max_width-8,static_cast<int>(extent.cx)+pad_*2);
                cell_widths.push_back(cell_width);
                if (!horizontal_) width=std::min(max_width,std::max(width,cell_width+8));
            }
            SelectObject(dc,old); ReleaseDC(popup_,dc);
            candidate_cells_.clear();
            int cell_x=4, cell_y=row_;
            for (const auto cell_width:cell_widths) {
                if (horizontal_) {
                    if (cell_x>4 && cell_x+cell_width>max_width-4) { cell_x=4; cell_y+=row_; }
                    candidate_cells_.push_back({cell_x,cell_y,cell_x+cell_width,cell_y+row_});
                    cell_x+=cell_width; width=std::max(width,cell_x+4);
                } else {
                    candidate_cells_.push_back({4,cell_y,width-4,cell_y+row_}); cell_y+=row_;
                }
            }
            const auto height=static_cast<int>(candidate_cells_.empty()?row_+pad_:candidate_cells_.back().bottom+pad_/2);
            auto x=std::clamp(static_cast<int>(caret_.left),static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right-width));
            auto y=caret_.bottom+3;
            if (y+height>monitor.rcWork.bottom) y=std::max(monitor.rcWork.top,caret_.top-height-3);
            SetWindowPos(popup_,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            InvalidateRect(popup_,nullptr,FALSE);
        } else if (popup_) {
            ShowWindow(popup_,SW_HIDE);
        }
        if (pending_) SetTimer(dispatch_,kPollTimer,35,nullptr); else KillTimer(dispatch_,kPollTimer);
    }
    void paint() {
        PAINTSTRUCT paint{}; HDC target=BeginPaint(popup_,&paint); RECT bounds{}; GetClientRect(popup_,&bounds);
        HDC dc=CreateCompatibleDC(target); HBITMAP buffer=CreateCompatibleBitmap(target,std::max(1L,bounds.right),std::max(1L,bounds.bottom));
        if (!dc || !buffer) { if (dc) DeleteDC(dc); if (buffer) DeleteObject(buffer); EndPaint(popup_,&paint); return; }
        auto oldbitmap=SelectObject(dc,buffer);
        auto background=CreateSolidBrush(RGB(250,251,255)); FillRect(dc,&bounds,background); DeleteObject(background);
        auto oldfont=SelectObject(dc,font_); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(45,49,70));
        const auto header=preedit_+(pending_?L"  …":L"");
        TextOutW(dc,pad_,pad_/2,header.data(),static_cast<int>(header.size()));
        auto begin=std::min<std::size_t>(page_*page_size_,candidates_.size());
        auto limit=std::min<std::size_t>(begin+page_size_,candidates_.size());
        for (auto i=begin;i<limit;++i) {
            if (i-begin>=candidate_cells_.size()) break;
            const auto cell=candidate_cells_[i-begin]; const auto saved=SaveDC(dc);
            IntersectClipRect(dc,cell.left,cell.top,cell.right,cell.bottom);
            auto y=static_cast<int>(cell.top);
            if (candidates_[i].model || i==selection_) {
                auto brush=CreateSolidBrush(candidates_[i].model?model_color_:RGB(226,234,255)); FillRect(dc,&cell,brush); DeleteObject(brush);
            }
            if (i==selection_) { auto brush=CreateSolidBrush(RGB(96,126,185)); FrameRect(dc,&cell,brush); DeleteObject(brush); }
            auto label=std::to_wstring((i-begin+1)%10)+L"  ";
            int x=cell.left+pad_/2; SetTextColor(dc,RGB(45,49,70)); TextOutW(dc,x,y,label.data(),static_cast<int>(label.size())); SIZE size{}; GetTextExtentPoint32W(dc,label.data(),static_cast<int>(label.size()),&size); x+=size.cx;
            IntersectClipRect(dc,cell.left,cell.top,cell.right-pad_/2,cell.bottom);
            auto draw=[&](std::wstring_view text,bool deleted) {
                SetTextColor(dc,deleted?RGB(155,158,170):RGB(30,34,56)); TextOutW(dc,x,y,text.data(),static_cast<int>(text.size()));
                GetTextExtentPoint32W(dc,text.data(),static_cast<int>(text.size()),&size);
                if (deleted) { auto pen=CreatePen(PS_SOLID,1,RGB(155,158,170)); auto oldpen=SelectObject(dc,pen); MoveToEx(dc,x,y+row_/3,nullptr); LineTo(dc,x+size.cx,y+row_/3); SelectObject(dc,oldpen); DeleteObject(pen); }
                x+=size.cx;
            };
            if (candidates_[i].pieces.empty()) draw(candidates_[i].text,false); else for (auto& piece:candidates_[i].pieces) draw(piece.first,piece.second);
            RestoreDC(dc,saved);
        }
        SelectObject(dc,oldfont); BitBlt(target,0,0,bounds.right,bounds.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,oldbitmap); DeleteObject(buffer); DeleteDC(dc); EndPaint(popup_,&paint);
    }
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<Service*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message==WM_NCCREATE) { self=static_cast<Service*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if (self) {
            if (window==self->mode_popup_) {
                if (message==WM_PAINT) {
                    PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint); RECT rect{}; GetClientRect(window,&rect);
                    FillRect(dc,&rect,GetSysColorBrush(COLOR_WINDOW));
                    auto icon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(self->chinese_?IDI_CHINESE:IDI_ENGLISH),IMAGE_ICON,32,32,0));
                    if (icon) { DrawIconEx(dc,8,8,icon,32,32,0,nullptr,DI_NORMAL); DestroyIcon(icon); }
                    EndPaint(window,&paint); return 0;
                }
                if (message==WM_TIMER) { KillTimer(window,1); ShowWindow(window,SW_HIDE); return 0; }
                if (message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
            }
            if (message==kReply) { std::unique_ptr<Json> value(reinterpret_cast<Json*>(lparam)); try { self->receive(std::move(*value)); } catch (...) { self->cancel(); } return 0; }
            if (message==kReplay) {
                std::unique_ptr<Replay> replay(reinterpret_cast<Replay*>(lparam));
                if (self->context_.Get()==replay->context.Get() && self->epoch_==replay->epoch && self->focused_) {
                    auto guard=self->request("guard");
                    if (self->snapshot(replay->context.Get(),guard)) {
                        ComPtr<ITfContextView> view; HWND target{};
                        if (SUCCEEDED(replay->context->GetActiveView(&view))) view->GetWnd(&target);
                        if (target && IsWindow(target) && GetWindowThreadProcessId(target,nullptr)==GetCurrentThreadId()) {
                            auto flags=replay->flags|kReplayMark;
                            PostMessageW(target,WM_KEYDOWN,replay->vk,flags);
                            PostMessageW(target,WM_KEYUP,replay->vk,flags|(static_cast<LPARAM>(1u)<<30)|(static_cast<LPARAM>(1u)<<31));
                        }
                    } else self->cancel();
                }
                return 0;
            }
            if (message==WM_TIMER && wparam==kPollTimer) {
                if (self->pending_ && !self->poll_queued_) {
                    auto poll=self->request("poll");
                    if (self->snapshot(self->context_.Get(),poll)) self->poll_queued_=self->enqueue(std::move(poll)); else self->cancel();
                }
                return 0;
            }
            if (message==WM_TIMER && wparam==kConfigTimer) {
                if (!self->config_queued_ && self->key_seq_<=self->rendered_seq_) self->config_queued_=self->enqueue({{"version",1},{"op","get_config"}});
                return 0;
            }
            if (message==WM_PAINT && window==self->popup_) { self->paint(); return 0; }
            if (message==WM_ERASEBKGND && window==self->popup_) return 1;
            if (message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
            if (message==WM_LBUTTONUP && window==self->popup_) {
                const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
                for (UINT i=0;i<self->candidate_cells_.size();++i)
                    if (PtInRect(&self->candidate_cells_[i],point)) { self->choose(self->page_*self->page_size_+i,false); break; }
                return 0;
            }
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
public:
    Service() { InterlockedIncrement(&objects); }
    ~Service() { InterlockedDecrement(&objects); }
    bool chinese() const { return chinese_; }
    void toggle() {
        POINT point{}; GetCursorPos(&point);
        if (caret_valid_) point={caret_.left,caret_.bottom};
        chinese_=!chinese_; cancel(); if (button_) button_->update(); update_mode();
        if (mode_popup_ && !(activation_&TF_TMAE_SECUREMODE)) {
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
            point.x=std::clamp(point.x,monitor.rcWork.left,monitor.rcWork.right-48);
            point.y=std::clamp(point.y+6,monitor.rcWork.top,monitor.rcWork.bottom-48);
            SetWindowPos(mode_popup_,HWND_TOPMOST,point.x,point.y,48,48,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            InvalidateRect(mode_popup_,nullptr,FALSE); SetTimer(mode_popup_,1,800,nullptr);
        }
    }
    void settings() {
        if (activation_&TF_TMAE_SECUREMODE) return;
        auto path=installed_directory()/L"EuphonySettings.exe";
        if (!std::filesystem::exists(path)) path=module_directory(module)/L"EuphonySettings.exe";
        if (!std::filesystem::exists(path)) path=module_directory(module).parent_path()/L"EuphonySettings.exe";
        auto command=L"\""+path.wstring()+L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,path.parent_path().c_str(),&startup,&process)) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    }
    void update_mode() {
        if (worker_ && !(activation_&TF_TMAE_SECUREMODE)) enqueue({{"version",1},{"op","set_mode"},{"chinese",chinese_}});
        ComPtr<ITfCompartmentMgr> manager; ComPtr<ITfCompartment> field;
        if (mgr_ && SUCCEEDED(mgr_.As(&manager)) && SUCCEEDED(manager->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,&field))) {
            VARIANT value{}; value.vt=VT_I4; value.lVal=chinese_?1:0; field->SetValue(client_,&value);
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if (!out) return E_POINTER; *out=nullptr;
        if (iid==IID_IUnknown || iid==IID_ITfTextInputProcessor || iid==IID_ITfTextInputProcessorEx) *out=static_cast<ITfTextInputProcessorEx*>(this);
        else if (iid==IID_ITfKeyEventSink) *out=static_cast<ITfKeyEventSink*>(this);
        else if (iid==IID_ITfCompositionSink) *out=static_cast<ITfCompositionSink*>(this);
        else if (iid==IID_ITfThreadMgrEventSink) *out=static_cast<ITfThreadMgrEventSink*>(this);
        else if (iid==IID_ITfThreadFocusSink) *out=static_cast<ITfThreadFocusSink*>(this);
        else if (iid==IID_ITfDisplayAttributeProvider) *out=static_cast<ITfDisplayAttributeProvider*>(this);
        else if (iid==IID_ITfTextEditSink) *out=static_cast<ITfTextEditSink*>(this);
        else if (iid==IID_ITfTextLayoutSink) *out=static_cast<ITfTextLayoutSink*>(this);
        else if (iid==IID_ITfUIElement || iid==IID_ITfCandidateListUIElement || iid==IID_ITfCandidateListUIElementBehavior) *out=static_cast<ITfCandidateListUIElementBehavior*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* mgr,TfClientId client) override { return ActivateEx(mgr,client,0); }
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* mgr,TfClientId client,DWORD flags) override {
        if (!mgr || mgr_) return E_INVALIDARG;
        if (flags&TF_TMAE_COMLESS) return E_NOTIMPL;
        mgr_=mgr; client_=client; activation_=flags; rotate();
        {
            std::lock_guard lock(class_mutex);
            if (!class_users) {
                WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance=module; cls.lpfnWndProc=window_proc; cls.lpszClassName=kWindowClass;
                RegisterClassExW(&cls); cls.lpszClassName=kPopupClass; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.style=CS_DROPSHADOW; RegisterClassExW(&cls);
            }
            ++class_users; classes_owned_=true;
        }
        dispatch_=CreateWindowExW(0,kWindowClass,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,module,this);
        popup_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,kPopupClass,L"Euphony",WS_POPUP|WS_BORDER,0,0,0,0,nullptr,nullptr,module,this);
        mode_popup_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,kPopupClass,L"Euphony mode",WS_POPUP|WS_BORDER,0,0,0,0,nullptr,nullptr,module,this);
        if (!dispatch_ || !popup_) { Deactivate(); return E_FAIL; }
        HDC dc=GetDC(popup_); auto dpi=GetDeviceCaps(dc,LOGPIXELSY); ReleaseDC(popup_,dc);
        row_=MulDiv(32,dpi,96); pad_=MulDiv(12,dpi,96);
        font_=CreateFontW(-MulDiv(16,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        worker_=start_worker(dispatch_); if (!worker_) { Deactivate(); return E_FAIL; }
        config_queued_=enqueue({{"version",1},{"op","get_config"}}); SetTimer(dispatch_,kConfigTimer,1000,nullptr);
        const char* stage="thread source";
        ComPtr<ITfSource> source; auto hr=mgr_.As(&source);
        if (SUCCEEDED(hr)) { stage="document event sink"; hr=source->AdviseSink(IID_ITfThreadMgrEventSink,static_cast<ITfThreadMgrEventSink*>(this),&events_cookie_); }
        if (SUCCEEDED(hr)) { stage="thread focus sink"; hr=source->AdviseSink(IID_ITfThreadFocusSink,static_cast<ITfThreadFocusSink*>(this),&focus_cookie_); }
        ComPtr<ITfKeystrokeMgr> keys;
        if (SUCCEEDED(hr)) { stage="keystroke manager"; hr=mgr_.As(&keys); }
        if (SUCCEEDED(hr)) { stage="key event sink"; hr=keys->AdviseKeyEventSink(client_,this,TRUE); }
        TF_PRESERVEDKEY toggle{VK_SHIFT,TF_MOD_ON_KEYUP};
        if (keys) keys->PreserveKey(client_,kToggle,&toggle,L"Euphony 中 / EN",15);
        mgr_.As(&ui_); mgr_.As(&langbar_);
        ComPtr<ITfCategoryMgr> categories;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&categories)))) categories->RegisterGUID(kPreeditAttribute,&attribute_);
        if (langbar_) { button_.Attach(new(std::nothrow) LanguageButton(this)); if (button_) langbar_->AddItem(button_.Get()); }
        ComPtr<ITfDocumentMgr> document; if (SUCCEEDED(mgr_->GetFocus(&document)) && document) { ComPtr<ITfContext> context; document->GetTop(&context); focus_context(context.Get()); }
        if (FAILED(hr)) { std::fprintf(stderr,"Euphony TSF activation: %s failed (0x%08lx)\n",stage,static_cast<unsigned long>(hr)); Deactivate(); return hr; }
        update_mode(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (!mgr_) return S_OK;
        hide(); focus_context(nullptr);
        if (worker_) { std::lock_guard lock(worker_->mutex); worker_->stop=true; worker_->window=nullptr; worker_->requests.clear(); worker_->wake.notify_one(); }
        worker_.reset();
        ComPtr<ITfKeystrokeMgr> keys;
        if (SUCCEEDED(mgr_.As(&keys))) { TF_PRESERVEDKEY toggle{VK_SHIFT,TF_MOD_ON_KEYUP}; keys->UnpreserveKey(kToggle,&toggle); keys->UnadviseKeyEventSink(client_); }
        ComPtr<ITfSource> source;
        if (SUCCEEDED(mgr_.As(&source))) {
            if (events_cookie_!=TF_INVALID_COOKIE) source->UnadviseSink(events_cookie_);
            if (focus_cookie_!=TF_INVALID_COOKIE) source->UnadviseSink(focus_cookie_);
        }
        events_cookie_=focus_cookie_=TF_INVALID_COOKIE;
        if (langbar_ && button_) langbar_->RemoveItem(button_.Get());
        if (button_) button_->detach(); button_.Reset(); langbar_.Reset(); ui_.Reset(); mgr_.Reset(); client_=TF_CLIENTID_NULL;
        if (dispatch_) { MSG message{}; while (PeekMessageW(&message,dispatch_,kReply,kReplay,PM_REMOVE)) { if (message.message==kReply) delete reinterpret_cast<Json*>(message.lParam); else delete reinterpret_cast<Replay*>(message.lParam); } DestroyWindow(dispatch_); dispatch_=nullptr; }
        if (mode_popup_) { DestroyWindow(mode_popup_); mode_popup_=nullptr; }
        if (popup_) { DestroyWindow(popup_); popup_=nullptr; } if (font_) { DeleteObject(font_); font_=nullptr; }
        if (classes_owned_) {
            std::lock_guard lock(class_mutex); classes_owned_=false;
            if (!--class_users) { UnregisterClassW(kPopupClass,module); UnregisterClassW(kWindowClass,module); }
        }
        config_queued_=false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override { if (!foreground) cancel(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context,WPARAM vk,LPARAM flags,BOOL* eaten) override {
        if (!eaten) return E_POINTER; *eaten=FALSE;
        if (flags&kReplayMark) return S_OK;
        if (vk==VK_SHIFT) shift_used_=false;
        else if (GetKeyState(VK_SHIFT)&0x8000) shift_used_=true;
        if (!chinese_ || failed_ || !focused_ || vk==VK_SHIFT) return S_OK;
        Json read=request("peek");
        focus_context(context);
        if (!snapshot(context,read)) { cancel(); return S_OK; }
        const auto key=key_name(vk,flags);
        const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        *eaten=consumes(key,active_,shift,ctrl,alt,full_width_,chinese_punctuation_) ||
            (!ctrl && !alt && key_seq_>rendered_seq_ && deferred_key(key));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context,WPARAM vk,LPARAM flags,BOOL* eaten) override {
        if (!eaten) return E_POINTER; *eaten=FALSE;
        if (flags&kReplayMark) return S_OK;
        if (vk==VK_SHIFT) { shift_used_=false; return S_OK; }
        shift_used_=true;
        if (!chinese_ || failed_ || !focused_) return S_OK;
        focus_context(context);
        auto value=request("key");
        if (!snapshot(context,value)) { cancel(); return S_OK; }
        auto key=key_name(vk,flags);
        bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        const bool barrier=!ctrl && !alt && key_seq_>rendered_seq_ && deferred_key(key);
        if (!consumes(key,active_,shift,ctrl,alt,full_width_,chinese_punctuation_) && !barrier) { if (ctrl || alt) cancel(); return S_OK; }
        value["key"]=key; value["shift"]=shift; value["ctrl"]=ctrl; value["alt"]=alt;
        const bool candidate_action=key=="Space" || (!shift && key.size()==1 && key[0]>='0' && key[0]<='9') ||
            key=="Tab" || key=="PageUp" || key=="PageDown" || key=="Up" || key=="Down" ||
            key=="-" || key=="=" || key=="_" || key=="+";
        if (candidate_action && !candidates_.empty()) value["candidate_generation"]=candidate_generation_;
        value["virtual_key"]=static_cast<std::uint64_t>(vk); value["key_flags"]=static_cast<std::uint64_t>(flags);
        const auto sequence=value["seq"].get<std::uint64_t>();
        if (!enqueue(std::move(value))) return S_OK;
        key_seq_=sequence;
        outstanding_.emplace_back(key_seq_,key);
        if (key.size()==1 && key[0]>='a' && key[0]<='z') { active_=true; recovery_+=wide(key); }
        else if (key=="Backspace" && !recovery_.empty()) recovery_.pop_back();
        else if (key=="Escape" || key=="Enter") active_=false;
        else if (key=="Space" && selection_<candidates_.size()) active_=candidates_[selection_].continues;
        else if (key.size()==1 && key[0]>='0' && key[0]<='9') {
            const auto index=page_*page_size_+static_cast<UINT>(key[0]=='0'?9:key[0]-'1');
            if (index<candidates_.size()) active_=candidates_[index].continues;
        }
        *eaten=TRUE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext*,WPARAM,LPARAM,BOOL* eaten) override { if (!eaten) return E_POINTER; *eaten=FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext*,WPARAM,LPARAM,BOOL* eaten) override { if (!eaten) return E_POINTER; *eaten=FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext*,REFGUID id,BOOL* eaten) override { if (!eaten) return E_POINTER; *eaten=id==kToggle&&!shift_used_; if (*eaten) toggle(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie,ITfComposition* composition) override {
        if (composition_.Get()==composition) { composition_.Reset(); hide(); rotate(); preedit_.clear(); } return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* doc) override { ComPtr<ITfDocumentMgr> current; if (context_) context_->GetDocumentMgr(&current); if (current.Get()==doc) focus_context(nullptr); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* document,ITfDocumentMgr*) override { ComPtr<ITfContext> context; if (document) document->GetTop(&context); focus_context(context.Get()); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override { ComPtr<ITfDocumentMgr> doc,current; if (mgr_) mgr_->GetFocus(&doc); context->GetDocumentMgr(&current); if (doc.Get()==current.Get()) focus_context(context); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override { if (context_.Get()==context) { ComPtr<ITfDocumentMgr> doc; ComPtr<ITfContext> top; if (mgr_) mgr_->GetFocus(&doc); if (doc) doc->GetTop(&top); focus_context(top.Get()==context?nullptr:top.Get()); } return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSetThreadFocus() override { focused_=true; update_mode(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnKillThreadFocus() override { focused_=false; if (mode_popup_) ShowWindow(mode_popup_,SW_HIDE); cancel(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context,TfEditCookie,ITfEditRecord* record) override {
        if (context!=context_.Get()) return S_OK;
        BOOL own{};
        if (SUCCEEDED(context->InWriteSession(client_,&own)) && own) return S_OK;
        BOOL selection_changed{}; record->GetSelectionStatus(&selection_changed);
        ComPtr<IEnumTfRanges> changed; ComPtr<ITfRange> range; ULONG count{};
        bool text_changed=SUCCEEDED(record->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT,nullptr,0,&changed)) &&
            changed && SUCCEEDED(changed->Next(1,&range,&count)) && count;
        if (selection_changed || text_changed) cancel();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* context,TfLayoutCode code,ITfContextView* view) override {
        if (context!=context_.Get()) return S_OK;
        if (code==TF_LC_DESTROY) { caret_valid_=false; if (popup_) ShowWindow(popup_,SW_HIDE); return S_OK; }
        if (!composition_ || !view) return S_OK;
        auto held=context_; ComPtr<ITfContextView> layout=view; const auto epoch=epoch_;
        edit(context,TF_ES_ASYNC|TF_ES_READ,[this,held,layout,epoch](TfEditCookie cookie) {
            if (context_.Get()!=held.Get() || epoch_!=epoch || !composition_) return S_OK;
            TF_SELECTION selected{}; ULONG count{};
            if (SUCCEEDED(held->GetSelection(cookie,TF_DEFAULT_SELECTION,1,&selected,&count)) && count) {
                ComPtr<ITfRange> range; range.Attach(selected.range);
                if (locate_caret(layout.Get(),cookie,range.Get())) update_ui();
            }
            return S_OK;
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* result) override { if (!result) return E_POINTER; *result=SysAllocString(L"Euphony candidates"); return *result?S_OK:E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* result) override { if (!result) return E_POINTER; *result=kCandidates; return S_OK; }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override { ui_allowed_=show!=FALSE; update_ui(); return S_OK; }
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* result) override { if (!result) return E_POINTER; *result=popup_&&IsWindowVisible(popup_); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* result) override { if (!result) return E_POINTER; *result=TF_CLUIE_COUNT|TF_CLUIE_SELECTION|TF_CLUIE_STRING|TF_CLUIE_PAGEINDEX|TF_CLUIE_CURRENTPAGE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** result) override { if (!result) return E_POINTER; *result=nullptr; return context_?context_->GetDocumentMgr(result):E_FAIL; }
    HRESULT STDMETHODCALLTYPE GetCount(UINT* result) override { if (!result) return E_POINTER; *result=static_cast<UINT>(candidates_.size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* result) override { if (!result) return E_POINTER; *result=selection_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetString(UINT index,BSTR* result) override { if (!result) return E_POINTER; *result=nullptr; if (index>=candidates_.size()) return E_INVALIDARG; *result=SysAllocStringLen(candidates_[index].text.data(),static_cast<UINT>(candidates_[index].text.size())); return *result?S_OK:E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* indexes,UINT size,UINT* count) override {
        if (!count || (size && !indexes)) return E_POINTER;
        *count=host_pages_.empty()?static_cast<UINT>((candidates_.size()+page_size_-1)/page_size_):static_cast<UINT>(host_pages_.size());
        for (UINT i=0;i<std::min(size,*count);++i) indexes[i]=host_pages_.empty()?i*page_size_:host_pages_[i];
        return size<*count?S_FALSE:S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* indexes,UINT count) override {
        if (!indexes || !count || count>candidates_.size() || indexes[0]!=0) return E_INVALIDARG;
        for (UINT i=0;i<count;++i) if (indexes[i]>=candidates_.size() || (i && indexes[i]<=indexes[i-1])) return E_INVALIDARG;
        host_pages_.assign(indexes,indexes+count); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* result) override {
        if (!result) return E_POINTER;
        *result=host_pages_.empty()?page_:static_cast<UINT>(std::upper_bound(host_pages_.begin(),host_pages_.end(),selection_)-host_pages_.begin()-1);
        return S_OK;
    }
    HRESULT choose(UINT index,bool highlight) {
        if (!context_ || index>=candidates_.size()) return E_INVALIDARG;
        auto value=request("key"); if (!snapshot(context_.Get(),value)) { cancel(); return E_FAIL; }
        value["key"]=highlight?"Highlight":"Select"; value["index"]=index; value["candidate_generation"]=candidate_generation_;
        const auto sequence=value["seq"].get<std::uint64_t>();
        if (!enqueue(std::move(value))) return E_FAIL;
        selection_=index; if (!highlight) active_=candidates_[index].continues; key_seq_=sequence; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override { return choose(index,true); }
    HRESULT STDMETHODCALLTYPE Finalize() override { return choose(selection_,false); }
    HRESULT STDMETHODCALLTYPE Abort() override { cancel(); return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** result) override { return enum_attributes(result); }
    HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid,ITfDisplayAttributeInfo** result) override { return get_attribute(guid,result); }
};

HRESULT LanguageButton::QueryInterface(REFIID iid,void** out) {
    if (!out) return E_POINTER; *out=nullptr;
    if (iid==IID_IUnknown || iid==IID_ITfLangBarItem || iid==IID_ITfLangBarItemButton) *out=static_cast<ITfLangBarItemButton*>(this);
    else if (iid==IID_ITfSource) *out=static_cast<ITfSource*>(this); else return E_NOINTERFACE;
    AddRef(); return S_OK;
}
HRESULT LanguageButton::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) return E_POINTER; *info={}; info->clsidService=kService; info->guidItem=kLanguageButton;
    info->dwStyle=TF_LBI_STYLE_BTN_BUTTON|TF_LBI_STYLE_SHOWNINTRAY;
    wcscpy_s(info->szDescription,L"Euphony 中 / EN"); return S_OK;
}
HRESULT LanguageButton::GetTooltipString(BSTR* result) {
    if (!result) return E_POINTER; *result=SysAllocString(owner_&&owner_->chinese()?L"Euphony · 中文（Shift 切换）":L"Euphony · English (Shift to switch)"); return *result?S_OK:E_OUTOFMEMORY;
}
HRESULT LanguageButton::OnClick(TfLBIClick click,POINT,const RECT*) { if (owner_) { if (click==TF_LBI_CLK_LEFT) owner_->toggle(); else owner_->settings(); } return S_OK; }
HRESULT LanguageButton::InitMenu(ITfMenu* menu) { return menu?menu->AddMenuItem(1,0,nullptr,nullptr,L"Euphony 设置",10,nullptr):E_POINTER; }
HRESULT LanguageButton::OnMenuSelect(UINT id) { if (id==1 && owner_) owner_->settings(); return S_OK; }
HRESULT LanguageButton::GetText(BSTR* result) { if (!result) return E_POINTER; *result=SysAllocString(owner_&&owner_->chinese()?L"中":L"EN"); return *result?S_OK:E_OUTOFMEMORY; }
HRESULT LanguageButton::GetIcon(HICON* result) {
    if (!result) return E_POINTER;
    // TSF owns the returned icon, so this is an independent resource instance.
    *result=reinterpret_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(owner_&&owner_->chinese()?IDI_CHINESE:IDI_ENGLISH),IMAGE_ICON,32,32,0));
    return *result?S_OK:E_FAIL;
}
HRESULT LanguageButton::AdviseSink(REFIID iid,IUnknown* object,DWORD* cookie) {
    if (!cookie || !object) return E_POINTER;
    if (iid!=IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    auto hr=object->QueryInterface(IID_PPV_ARGS(&sink_)); if (SUCCEEDED(hr)) *cookie=1; return hr;
}
HRESULT LanguageButton::UnadviseSink(DWORD cookie) { if (cookie!=1 || !sink_) return CONNECT_E_NOCONNECTION; sink_.Reset(); return S_OK; }
}

HRESULT create_service(REFIID iid,void** result) {
    if (!result) return E_POINTER; *result=nullptr;
    auto* service=new(std::nothrow) Service;
    if (!service) return E_OUTOFMEMORY;
    auto hr=service->QueryInterface(iid,result); service->Release(); return hr;
}
}
