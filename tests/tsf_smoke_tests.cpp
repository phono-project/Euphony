#include "tsf/guids.h"
#include "tsf_text_store.h"
#include "tsf_thread_adapter.h"
#include "common/ipc.h"
#include <msctf.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>

using Microsoft::WRL::ComPtr;
namespace {
WNDPROC popup_original{};
bool misplaced_popup = false;
LRESULT CALLBACK observe_popup(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if (message==WM_WINDOWPOSCHANGING) {
        const auto* position=reinterpret_cast<WINDOWPOS*>(lp);
        if (!(position->flags&SWP_NOMOVE) && !(position->flags&SWP_HIDEWINDOW) && (position->x<90 || position->y<90)) misplaced_popup=true;
    }
    return CallWindowProcW(popup_original,window,message,wp,lp);
}
void snapshot_popup(HWND window,const wchar_t* name) {
    RECT rect{}; GetClientRect(window,&rect);
    HDC source=GetDC(window), target=CreateCompatibleDC(source);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=rect.right;
    info.bmiHeader.biHeight=-rect.bottom; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels{}; HBITMAP bitmap=CreateDIBSection(source,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!bitmap || !target) throw std::runtime_error("Cannot capture candidate window");
    auto old=SelectObject(target,bitmap); SendMessageW(window,WM_PAINT,0,0); BitBlt(target,0,0,rect.right,rect.bottom,source,0,0,SRCCOPY);
    BITMAPFILEHEADER header{}; header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+rect.right*rect.bottom*4;
    std::ofstream file(euphony::module_directory()/name,std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header),sizeof(header)); file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    file.write(static_cast<const char*>(pixels),rect.right*rect.bottom*4);
    SelectObject(target,old); DeleteObject(bitmap); DeleteDC(target); ReleaseDC(window,source);
}
}
void check(HRESULT hr,const char* message) { if (FAILED(hr)) { std::ostringstream detail; detail<<message<<" (HRESULT 0x"<<std::hex<<static_cast<unsigned long>(hr)<<')'; throw std::runtime_error(detail.str()); } }
template<class Predicate> void pump_until(Predicate predicate,DWORD timeout,const char* message) {
    const auto deadline=GetTickCount64()+timeout;
    while (!predicate()) {
        MSG queued{};
        while (PeekMessageW(&queued,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&queued); DispatchMessageW(&queued); }
        if (GetTickCount64()>=deadline) throw std::runtime_error(message);
        MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
    }
}
void integration(ITfTextInputProcessorEx* service,ITfKeyEventSink* keys,ITfCandidateListUIElementBehavior* candidates) {
    ComPtr<ITfThreadMgrEx> manager; check(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager)),"Thread manager unavailable");
    TfClientId client{}; check(manager->ActivateEx(&client,TF_TMAE_NOACTIVATETIP|TF_TMAE_NOACTIVATEKEYBOARDLAYOUT),"Thread activation failed");
    ComPtr<ITfClientId> identifiers; check(manager.As(&identifiers),"Service client identifiers unavailable");
    TfClientId service_client{}; check(identifiers->GetClientId(euphony::tsf::kService,&service_client),"Service client identifier unavailable");
    ComPtr<TestThreadAdapter> adapter; adapter.Attach(new TestThreadAdapter(manager.Get()));
    ComPtr<TestTextStore> store; store.Attach(new TestTextStore);
    euphony::IpcClient broker(15000);
    auto initial_config=broker.call({{"version",1},{"op","get_config"}}).at("config");
    ComPtr<ITfSource> source; check(manager.As(&source),"Thread events unavailable");
    DWORD ui_cookie{}; check(source->AdviseSink(IID_ITfUIElementSink,static_cast<ITfUIElementSink*>(store.Get()),&ui_cookie),"UIless host unavailable");
    ComPtr<ITfDocumentMgr> document; check(manager->CreateDocumentMgr(&document),"Document unavailable");
    ComPtr<ITfContext> context; TfEditCookie cookie{};
    check(document->CreateContext(client,0,static_cast<ITextStoreACP*>(store.Get()),&context,&cookie),"Text store context unavailable");
    check(document->Push(context.Get()),"Context push failed");
    try {
        for (int cycle=0;cycle<2;++cycle) {
            check(service->ActivateEx(adapter.Get(),service_client,0),"Service activation failed");
            ComPtr<ITfActiveLanguageProfileNotifySink> selected; check(service->QueryInterface(IID_PPV_ARGS(&selected)),"Profile sink missing");
            check(selected->OnActivated(euphony::tsf::kService,euphony::tsf::kProfile,TRUE),"Test profile selection failed");
            check(manager->SetFocus(document.Get()),"Test document focus failed");
            BOOL eaten{}; check(keys->OnTestKeyDown(context.Get(),'N',0,&eaten),"Key test failed");
            if (!eaten) throw std::runtime_error("Pinyin start key not consumed");
            check(keys->OnKeyDown(context.Get(),'N',0,&eaten),"First pinyin key failed");
            check(keys->OnKeyDown(context.Get(),'I',0,&eaten),"Second pinyin key failed");
            if (!eaten) throw std::runtime_error("Pinyin key not consumed");
            pump_until([&] { return store->text.ends_with(L"ni"); },8000,"Asynchronous preedit never reached real TSF text store");
            UINT count{};
            pump_until([&] { candidates->GetCount(&count); return count>0; },8000,"Candidate list unavailable");
            if (!store->started) throw std::runtime_error("TSF composition was not started");
            BSTR first_candidate{}; check(candidates->GetString(0,&first_candidate),"Initial candidate unavailable");
            const auto first=euphony::utf8(std::wstring_view(first_candidate,SysStringLen(first_candidate))); SysFreeString(first_candidate);
            const auto ended_before=store->ended;
            check(keys->OnKeyDown(context.Get(),VK_SPACE,0,&eaten),"Candidate commit key failed");
            pump_until([&] { candidates->GetCount(&count); return store->ended>ended_before && count==0 && !store->text.empty(); },5000,"Space did not finish its Chinese candidate commit");
            if (euphony::utf8(store->text)!=first) {
                std::ostringstream detail; detail<<"Space committed text differs from displayed candidate: text="<<euphony::utf8(store->text)<<", first="<<first<<", ended="<<store->ended<<", units=";
                for (wchar_t unit:store->text) detail<<std::hex<<static_cast<unsigned>(unit)<<' ';
                throw std::runtime_error(detail.str());
            }
            const auto committed=store->text;
            check(keys->OnKeyDown(context.Get(),'N',0,&eaten),"Next composition start failed");
            check(keys->OnKeyDown(context.Get(),'I',0,&eaten),"Next composition key failed");
            pump_until([&] { return store->text==committed+L"ni"; },5000,"Composition overwrote the previously committed Chinese");
            check(keys->OnKeyDown(context.Get(),VK_ESCAPE,0,&eaten),"Composition cancellation key failed");
            pump_until([&] { return store->text==committed; },5000,"Escape did not remove only uncommitted preedit");
            store->clear();
            if (cycle==0) {
                check(keys->OnTestKeyDown(context.Get(),VK_OEM_5,0,&eaten),"Backslash ownership failed");
                if (!eaten) throw std::runtime_error("Idle backslash did not reach Chinese punctuation");
                check(keys->OnKeyDown(context.Get(),VK_OEM_5,0,&eaten),"Backslash input failed");
                pump_until([&] { return store->text==L"、"; },5000,"Backslash did not insert enumeration comma");
                store->clear();
                check(keys->OnKeyDown(context.Get(),VK_SHIFT,0,&eaten),"Shift press failed");
                check(keys->OnPreservedKey(context.Get(),euphony::tsf::kToggle,&eaten),"English toggle failed");
                if (!eaten) throw std::runtime_error("Mode toggle was not accepted");
                check(keys->OnTestKeyDown(context.Get(),'N',0,&eaten),"English ownership check failed");
                if (eaten) throw std::runtime_error("English mode consumed a letter");
                HWND hint{};
                EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM target)->BOOL {
                    wchar_t title[128]{}; GetWindowTextW(window,title,128);
                    if (!wcscmp(title,L"Euphony mode")) { *reinterpret_cast<HWND*>(target)=window; return FALSE; } return TRUE;
                },reinterpret_cast<LPARAM>(&hint));
                if (!hint || !IsWindowVisible(hint)) throw std::runtime_error("Mode icon hint was not shown");
                snapshot_popup(hint,L"mode-english.bmp");
                pump_until([&] { return !IsWindowVisible(hint); },2000,"Mode hint did not dismiss");
                check(keys->OnKeyDown(context.Get(),VK_SHIFT,0,&eaten),"Shift press failed");
                check(keys->OnPreservedKey(context.Get(),euphony::tsf::kToggle,&eaten),"Chinese toggle failed");
                snapshot_popup(hint,L"mode-chinese.bmp");
                check(keys->OnPreservedKey(context.Get(),euphony::tsf::kControlSpace,&eaten),"Ctrl Space English toggle failed");
                if (!eaten) throw std::runtime_error("Ctrl Space was not accepted");
                check(keys->OnTestKeyDown(context.Get(),'N',0,&eaten),"Ctrl Space English ownership check failed");
                if (eaten) throw std::runtime_error("Ctrl Space did not enter English mode");
                check(keys->OnPreservedKey(context.Get(),euphony::tsf::kControlSpace,&eaten),"Ctrl Space Chinese toggle failed");
                check(keys->OnTestKeyDown(context.Get(),'N',0,&eaten),"Ctrl Space Chinese ownership check failed");
                if (!eaten) throw std::runtime_error("Ctrl Space did not return to Chinese mode");
                auto type_ni=[&] {
                    check(keys->OnKeyDown(context.Get(),'N',0,&eaten),"Rapid test composition start failed");
                    check(keys->OnKeyDown(context.Get(),'I',0,&eaten),"Rapid test composition key failed");
                    pump_until([&] { candidates->GetCount(&count); return store->text==L"ni" && count>1; },5000,"Rapid test candidates unavailable");
                    BSTR shown{}; check(candidates->GetString(0,&shown),"Rapid test first candidate unavailable");
                    std::wstring result(shown,SysStringLen(shown)); SysFreeString(shown);
                    if (result.empty()) throw std::runtime_error("Rapid test first candidate is empty");
                    return result;
                };
                const auto backspace_candidate=type_ni();
                auto previous_character=[](const std::wstring& value) {
                    auto index=value.size()-1;
                    if (index && value[index]>=0xdc00 && value[index]<=0xdfff && value[index-1]>=0xd800 && value[index-1]<=0xdbff) --index;
                    return index;
                };
                const auto backspace_expected=backspace_candidate.substr(0,previous_character(backspace_candidate));
                check(keys->OnKeyDown(context.Get(),VK_SPACE,0,&eaten),"Rapid commit failed");
                check(keys->OnTestKeyDown(context.Get(),VK_BACK,0,&eaten),"Rapid Backspace ownership test failed");
                if (!eaten) throw std::runtime_error("Backspace passed through before queued commit");
                check(keys->OnKeyDown(context.Get(),VK_BACK,0,&eaten),"Rapid Backspace failed");
                const auto backspace_ended=store->ended;
                pump_until([&] { return store->ended>backspace_ended && store->text==backspace_expected; },5000,"Space then Backspace did not remove the last newly committed character");
                store->clear();
                const auto left_candidate=type_ni();
                const auto left_boundary=previous_character(left_candidate);
                const auto left_expected=left_candidate.substr(0,left_boundary)+L"h"+left_candidate.substr(left_boundary);
                check(keys->OnKeyDown(context.Get(),VK_SPACE,0,&eaten),"Rapid cursor commit failed");
                check(keys->OnKeyDown(context.Get(),VK_LEFT,0,&eaten),"Rapid Left failed");
                if (!eaten) throw std::runtime_error("Left passed through before queued commit");
                check(keys->OnKeyDown(context.Get(),'H',0,&eaten),"Composition after rapid Left failed");
                pump_until([&] { return store->text==left_expected; },5000,"Rapid Left did not order insertion at the previous character boundary");
                check(keys->OnKeyDown(context.Get(),VK_ESCAPE,0,&eaten),"Rapid Left cleanup failed");
                pump_until([&] { return store->text==left_candidate; },5000,"Rapid Left cleanup removed committed text");
                store->clear();
                type_ni();
                UINT pages[]{0,1}; check(candidates->SetPageIndex(pages,2),"UIless page boundaries failed");
                UINT received[2]{},page_count{}; check(candidates->GetPageIndex(received,2,&page_count),"UIless page boundaries unavailable");
                if (page_count!=2 || received[0]!=0 || received[1]!=1) throw std::runtime_error("UIless host page boundaries were not preserved");
                UINT invalid_pages[]{0,0};
                if (candidates->SetPageIndex(invalid_pages,2)!=E_INVALIDARG) throw std::runtime_error("Invalid UIless page boundaries were accepted");
                BSTR expected{}; check(candidates->GetString(1,&expected),"Second UIless candidate unavailable");
                std::wstring second(expected,SysStringLen(expected)); SysFreeString(expected);
                check(candidates->SetSelection(1),"UIless selection failed");
                check(candidates->Finalize(),"Immediate UIless finalize failed");
                pump_until([&] { return store->text==second; },5000,"UIless SetSelection then Finalize committed the wrong index");
                store->clear();
                check(keys->OnKeyDown(context.Get(),'N',0,&eaten),"Pending privacy composition failed");
                store->password(true);
                const auto privacy_deadline=GetTickCount64()+500;
                pump_until([&] { return GetTickCount64()>=privacy_deadline; },2000,"Privacy wait failed");
                if (!store->text.empty()) throw std::runtime_error("Late preedit entered a field that became a password");
                store->password(false);
            }
            store->password(true);
            check(keys->OnTestKeyDown(context.Get(),'N',0,&eaten),"Sensitive key test failed");
            if (eaten) throw std::runtime_error("Password field consumed pinyin or read input context");
            store->password(false);
            if (cycle==1) {
                // Exercise the service's own popup with a real TSF host whose
                // layout disappears while SetText runs, as browsers commonly do.
                store->clear(); store->show_candidates=true; store->defer_layout=true;
                HWND popup{};
                EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM target)->BOOL {
                    wchar_t type[128]{},title[128]{}; GetClassNameW(window,type,128); GetWindowTextW(window,title,128);
                    if (!wcscmp(type,L"Euphony.Tsf.Candidates.v1") && !wcscmp(title,L"Euphony")) { *reinterpret_cast<HWND*>(target)=window; return FALSE; }
                    return TRUE;
                },reinterpret_cast<LPARAM>(&popup));
                if (!popup) throw std::runtime_error("Candidate popup missing");
                popup_original=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(popup,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(observe_popup)));
                try {
                    for (const auto* theme : {"light","dark"}) for (const auto* layout : {"vertical","horizontal"}) {
                        broker.call({{"version",1},{"op","set_config"},{"config",{{"theme",theme},{"show_preedit",true},{"candidate_layout",layout},{"page_size",5},{"user_candidate_color","#FBE4EC"}}}});
                        store->layout_state=1;
                        check(keys->OnKeyDown(context.Get(),'N',0,&eaten),"Popup composition start failed");
                        check(keys->OnKeyDown(context.Get(),'I',0,&eaten),"Popup composition key failed");
                        pump_until([&] { candidates->GetCount(&count); return store->text==L"ni" && count>=5; },8000,"Popup candidates unavailable");
                        check(candidates->Show(TRUE),"Popup show request failed");
                        if (IsWindowVisible(popup)) throw std::runtime_error("Popup shown before any valid layout");
                        store->publish_layout(0);
                        pump_until([&] { return IsWindowVisible(popup)!=FALSE; },5000,"Popup did not appear after layout became ready");
                        RECT before{}; GetWindowRect(popup,&before);
                        if (before.left<90 || before.top<90) throw std::runtime_error("Popup first appeared at the origin");
                        snapshot_popup(popup, std::string_view(layout)=="vertical"?L"candidates-vertical.bmp":L"candidates-horizontal.bmp");
                        if (std::string_view(layout)=="vertical" && before.bottom-before.top<170) throw std::runtime_error("Vertical candidate rows missing");
                        if (std::string_view(layout)=="horizontal" && before.bottom-before.top>100) throw std::runtime_error("Horizontal candidates were not placed side by side");
                        store->publish_layout(2);
                        const auto wait=GetTickCount64()+150;
                        pump_until([&] { return GetTickCount64()>=wait; },2000,"Empty layout wait failed");
                        RECT after{}; GetWindowRect(popup,&after);
                        if (!EqualRect(&before,&after) || !IsWindowVisible(popup)) throw std::runtime_error("Empty layout moved or hid an already positioned popup");
                        check(keys->OnKeyDown(context.Get(),'H',0,&eaten),"Popup incremental key failed");
                        pump_until([&] { return store->text==L"nih"; },5000,"Incremental popup preedit failed");
                        GetWindowRect(popup,&after);
                        if (after.left!=before.left || after.top!=before.top) throw std::runtime_error("Missing layout moved popup after an incremental key");
                        store->publish_layout(0);
                        pump_until([&] { RECT moved{}; GetWindowRect(popup,&moved); return moved.left>before.left; },5000,"Popup did not track its next valid caret position");
                        check(keys->OnKeyDown(context.Get(),VK_ESCAPE,0,&eaten),"Popup cleanup failed");
                        pump_until([&] { return store->text.empty() && !IsWindowVisible(popup); },5000,"Popup did not close on cancellation");
                        store->layout_state=0;
                        check(keys->OnKeyDown(context.Get(),'N',0,&eaten),"Mouse test start failed");
                        check(keys->OnKeyDown(context.Get(),'I',0,&eaten),"Mouse test key failed");
                        pump_until([&] { candidates->GetCount(&count); return store->text==L"ni" && count>=5; },5000,"Mouse test candidates unavailable");
                        store->publish_layout(0);
                        pump_until([&] { return IsWindowVisible(popup)!=FALSE; },5000,"Mouse test popup unavailable");
                        UpdateWindow(popup);
                        // Locate the first highlighted cell in the actual rendered
                        // image, then click its neighbor in the chosen direction.
                        HDC pixels=GetDC(popup); RECT client_rect{}; GetClientRect(popup,&client_rect);
                        const auto selected_color=std::string_view(theme)=="dark"?RGB(24,59,102):RGB(226,234,255);
                        int first_y=0;
                        while (first_y<client_rect.bottom && GetPixel(pixels,5,first_y)!=selected_color) ++first_y;
                        int next_x=5,next_y=first_y;
                        while (next_x<client_rect.right && GetPixel(pixels,next_x,first_y+1)==selected_color) ++next_x;
                        while (next_y<client_rect.bottom && GetPixel(pixels,5,next_y)==selected_color) ++next_y;
                        ReleaseDC(popup,pixels);
                        if (first_y>=client_rect.bottom) throw std::runtime_error("Selected candidate theme background missing");
                        BSTR chosen{}; check(candidates->GetString(1,&chosen),"Mouse target unavailable");
                        std::wstring expected_click(chosen,SysStringLen(chosen)); SysFreeString(chosen);
                        const auto x=std::string_view(layout)=="horizontal"?next_x+8:14;
                        const auto y=std::string_view(layout)=="horizontal"?first_y+8:next_y+8;
                        SendMessageW(popup,WM_LBUTTONUP,0,MAKELPARAM(x,y));
                        pump_until([&] { return store->text==expected_click && !IsWindowVisible(popup); },5000,"Candidate click selected the wrong item for its layout");
                        store->clear();
                    }
                    if (misplaced_popup) throw std::runtime_error("Candidate popup moved through the screen origin");
                } catch (...) { SetWindowLongPtrW(popup,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(popup_original)); throw; }
                SetWindowLongPtrW(popup,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(popup_original));
                store->show_candidates=false; store->defer_layout=false; store->layout_state=0;
            }
            check(service->Deactivate(),"Service deactivation failed");
        }
        if (!store->ended) throw std::runtime_error("TSF composition never ended");
    } catch (...) { broker.call({{"version",1},{"op","set_config"},{"config",initial_config}}); service->Deactivate(); document->Pop(TF_POPF_ALL); source->UnadviseSink(ui_cookie); manager->Deactivate(); throw; }
    broker.call({{"version",1},{"op","set_config"},{"config",initial_config}});
    // Keep an activated frontend and its real configuration timer alive after
    // shutdown. Neither focus notifications nor cleanup may recreate the broker.
    check(service->ActivateEx(adapter.Get(),service_client,0),"Exit regression activation failed");
    ComPtr<ITfThreadFocusSink> thread_focus; check(service->QueryInterface(IID_PPV_ARGS(&thread_focus)),"Thread focus sink missing");
    ComPtr<ITfActiveLanguageProfileNotifySink> profile; check(service->QueryInterface(IID_PPV_ARGS(&profile)),"Profile activation sink missing");
    check(profile->OnActivated(euphony::tsf::kService,euphony::tsf::kProfile,TRUE),"Exit test profile selection failed");
    auto wait_messages=[](DWORD duration) {
        const auto until=GetTickCount64()+duration;
        pump_until([&] { return GetTickCount64()>=until; },duration+1000,"Lifecycle observation timed out");
    };
    const auto pipe=euphony::pipe_name();
    auto broker_present=[&] { return WaitNamedPipeW(pipe.c_str(),1) || GetLastError()==ERROR_SEM_TIMEOUT; };
    wait_messages(300);
    broker.call({{"version",1},{"op","shutdown"}});
    pump_until([&] { return !broker_present(); },5000,"Broker did not stop");
    check(thread_focus->OnKillThreadFocus(),"Exit focus loss failed");
    check(keys->OnSetFocus(FALSE),"Exit keyboard focus loss failed");
    check(thread_focus->OnSetThreadFocus(),"Exit focus restore failed");
    check(keys->OnSetFocus(TRUE),"Exit keyboard focus restore failed");
    wait_messages(3500);
    if (broker_present()) throw std::runtime_error("Passive frontend requests restarted the stopped broker");
    check(profile->OnActivated(euphony::tsf::kService,euphony::tsf::kProfile,FALSE),"Profile switch away failed");
    check(profile->OnActivated(euphony::tsf::kService,euphony::tsf::kProfile,TRUE),"Profile switch back failed");
    pump_until([&] { return broker_present(); },15000,"Selecting Euphony again did not restart the broker");
    euphony::IpcClient(15000,false).call({{"version",1},{"op","shutdown"}});
    pump_until([&] { return !broker_present(); },5000,"Reselected broker did not stop");
    check(service->Deactivate(),"Exit cleanup failed");
    wait_messages(3000);
    if (broker_present()) throw std::runtime_error("Frontend deactivation restarted the stopped broker");
    document->Pop(TF_POPF_ALL); source->UnadviseSink(ui_cookie); manager->Deactivate();
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    HMODULE dll=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { std::cerr<<"Cannot load TSF DLL: "<<GetLastError()<<'\n'; CoUninitialize(); return 1; }
    using GetFactory=HRESULT(__stdcall*)(REFCLSID,REFIID,void**);
    using Unload=HRESULT(__stdcall*)();
    auto get=reinterpret_cast<GetFactory>(GetProcAddress(dll,"DllGetClassObject"));
    auto unload=reinterpret_cast<Unload>(GetProcAddress(dll,"DllCanUnloadNow"));
    int result=0;
    try {
        if (!get || !unload || !GetProcAddress(dll,"DllRegisterServer") || !GetProcAddress(dll,"DllUnregisterServer")) throw std::runtime_error("COM exports missing");
        {
            ComPtr<IClassFactory> factory; check(get(euphony::tsf::kService,IID_PPV_ARGS(&factory)),"Class factory unavailable");
            ComPtr<ITfTextInputProcessorEx> service; check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service)),"TSF service unavailable");
            ComPtr<ITfKeyEventSink> keys; check(service.As(&keys),"Keyboard sink unavailable");
            ComPtr<ITfCompositionSink> composition; check(service.As(&composition),"Composition sink unavailable");
            ComPtr<ITfThreadMgrEventSink> document; check(service.As(&document),"Document sink unavailable");
            ComPtr<ITfThreadFocusSink> focus; check(service.As(&focus),"Thread focus sink unavailable");
            ComPtr<ITfCandidateListUIElementBehavior> candidates; check(service.As(&candidates),"UIless candidates unavailable");
            ComPtr<ITfDisplayAttributeProvider> attributes; check(service.As(&attributes),"Display attribute provider unavailable");
            ComPtr<IEnumTfDisplayAttributeInfo> list; check(attributes->EnumDisplayAttributeInfo(&list),"Preedit attributes unavailable");
            ComPtr<ITfDisplayAttributeInfo> attribute; ULONG fetched{};
            check(list->Next(1,&attribute,&fetched),"Cannot enumerate preedit style");
            if (fetched!=1) throw std::runtime_error("Expected a preedit display attribute");
            GUID id{}; check(attribute->GetGUID(&id),"Preedit GUID unavailable");
            if (id!=euphony::tsf::kPreeditAttribute) throw std::runtime_error("Incorrect preedit GUID");
            TF_DISPLAYATTRIBUTE style{}; check(attribute->GetAttributeInfo(&style),"Preedit style unavailable");
            if (style.lsStyle!=TF_LS_SOLID || style.bAttr!=TF_ATTR_INPUT) throw std::runtime_error("Incorrect composition style");
            if (unload()!=S_FALSE) throw std::runtime_error("DLL unloaded with live COM interfaces");
            integration(service.Get(),keys.Get(),candidates.Get());
            check(service->Deactivate(),"Inactive service cleanup failed");
        }
        pump_until([&] { return unload()==S_OK; },5000,"COM or worker references leaked after destruction");
        std::cout<<"TSF exports, activation lifecycle, real text-store composition, password privacy, UIless candidates and shutdown without passive restart passed without registration\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; result=1; }
    try { euphony::IpcClient(500,false).call({{"version",1},{"op","shutdown"}}); } catch (...) {}
    FreeLibrary(dll); CoUninitialize(); return result;
}
