#pragma once
#include <msctf.h>
#include <wrl/client.h>

// Windows' foreground-key manager requires a registered/activated TIP. The
// development test cannot register a keyboard on the user's OS, so only this
// registration boundary is adapted. Documents, ranges, composition, locks,
// edit notifications, candidate UI management, and all IPC remain real.
class TestThreadAdapter final : public ITfThreadMgr, public ITfKeystrokeMgr {
    LONG refs_=1;
    Microsoft::WRL::ComPtr<ITfThreadMgr> real_;
    Microsoft::WRL::ComPtr<ITfKeyEventSink> sink_;
public:
    explicit TestThreadAdapter(ITfThreadMgr* manager) : real_(manager) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if (!out) return E_POINTER; *out=nullptr;
        if (iid==IID_IUnknown || iid==IID_ITfThreadMgr) *out=static_cast<ITfThreadMgr*>(this);
        else if (iid==IID_ITfKeystrokeMgr) *out=static_cast<ITfKeystrokeMgr*>(this);
        else return real_->QueryInterface(iid,out);
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Activate(TfClientId* id) override { return real_->Activate(id); }
    HRESULT STDMETHODCALLTYPE Deactivate() override { return real_->Deactivate(); }
    HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr** out) override { return real_->CreateDocumentMgr(out); }
    HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs** out) override { return real_->EnumDocumentMgrs(out); }
    HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr** out) override { return real_->GetFocus(out); }
    HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr* document) override { return real_->SetFocus(document); }
    HRESULT STDMETHODCALLTYPE AssociateFocus(HWND window,ITfDocumentMgr* next,ITfDocumentMgr** old) override { return real_->AssociateFocus(window,next,old); }
    HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL* out) override { if (!out) return E_POINTER; *out=TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID id,ITfFunctionProvider** out) override { return real_->GetFunctionProvider(id,out); }
    HRESULT STDMETHODCALLTYPE EnumFunctionProviders(IEnumTfFunctionProviders** out) override { return real_->EnumFunctionProviders(out); }
    HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr** out) override { return real_->GetGlobalCompartment(out); }
    HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId,ITfKeyEventSink* sink,BOOL) override { if (!sink) return E_INVALIDARG; sink_=sink; return S_OK; }
    HRESULT STDMETHODCALLTYPE UnadviseKeyEventSink(TfClientId) override { sink_.Reset(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetForeground(CLSID* out) override { if (!out) return E_POINTER; *out=GUID_NULL; return S_OK; }
    HRESULT event(WPARAM vk,LPARAM flags,BOOL* eaten,bool test,bool up) {
        if (!sink_) return E_FAIL;
        Microsoft::WRL::ComPtr<ITfDocumentMgr> document; Microsoft::WRL::ComPtr<ITfContext> context;
        real_->GetFocus(&document); if (document) document->GetTop(&context);
        if (up) return test?sink_->OnTestKeyUp(context.Get(),vk,flags,eaten):sink_->OnKeyUp(context.Get(),vk,flags,eaten);
        return test?sink_->OnTestKeyDown(context.Get(),vk,flags,eaten):sink_->OnKeyDown(context.Get(),vk,flags,eaten);
    }
    HRESULT STDMETHODCALLTYPE TestKeyDown(WPARAM vk,LPARAM flags,BOOL* eaten) override { return event(vk,flags,eaten,true,false); }
    HRESULT STDMETHODCALLTYPE TestKeyUp(WPARAM vk,LPARAM flags,BOOL* eaten) override { return event(vk,flags,eaten,true,true); }
    HRESULT STDMETHODCALLTYPE KeyDown(WPARAM vk,LPARAM flags,BOOL* eaten) override { return event(vk,flags,eaten,false,false); }
    HRESULT STDMETHODCALLTYPE KeyUp(WPARAM vk,LPARAM flags,BOOL* eaten) override { return event(vk,flags,eaten,false,true); }
    HRESULT STDMETHODCALLTYPE GetPreservedKey(ITfContext*,const TF_PRESERVEDKEY*,GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsPreservedKey(REFGUID,const TF_PRESERVEDKEY*,BOOL* out) override { if (!out) return E_POINTER; *out=TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId,REFGUID,const TF_PRESERVEDKEY*,const WCHAR*,ULONG) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnpreserveKey(REFGUID,const TF_PRESERVEDKEY*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPreservedKeyDescription(REFGUID,const WCHAR*,ULONG) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPreservedKeyDescription(REFGUID,BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SimulatePreservedKey(ITfContext* context,REFGUID id,BOOL* eaten) override { return sink_?sink_->OnPreservedKey(context,id,eaten):E_FAIL; }
};
