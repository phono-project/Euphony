#pragma once
#include <msctf.h>
#include <wrl/client.h>
#include <textstor.h>
#include <string>

// Match Terminal's context-owner host: TSF owns the transient text buffer,
// with no ITextStoreACP supplied by the application.
class TestConsoleOwner final : public ITfContextOwner, public ITfContextOwnerCompositionSink, public ITfTextEditSink {
    LONG refs_=1;
    bool pending_=false;
    class Update final : public ITfEditSession {
        LONG refs_=1; TestConsoleOwner* owner_;
    public:
        explicit Update(TestConsoleOwner* owner) : owner_(owner) { owner_->AddRef(); }
        ~Update() { owner_->pending_=false;owner_->Release(); }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
            if (!out) return E_POINTER;*out=nullptr;
            if (iid!=IID_IUnknown && iid!=IID_ITfEditSession) return E_NOINTERFACE;
            *out=static_cast<ITfEditSession*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
        ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n; }
        HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override { return owner_->read(cookie); }
    };
    void update() {
        if (pending_ || !context) return;
        pending_=true; HRESULT result{};auto* job=new Update(this);
        context->RequestEditSession(client,job,TF_ES_ASYNC|TF_ES_READWRITE,&result);job->Release();
    }
public:
    UINT started{},ended{};
    ITfContext* context=nullptr;
    TfClientId client{};
    std::wstring preview,output;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if (!out) return E_POINTER; *out=nullptr;
        if (iid==IID_IUnknown || iid==IID_ITfContextOwner) *out=static_cast<ITfContextOwner*>(this);
        else if (iid==IID_ITfContextOwnerCompositionSink) *out=static_cast<ITfContextOwnerCompositionSink*>(this);
        else if (iid==IID_ITfTextEditSink) *out=static_cast<ITfTextEditSink*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(const POINT*,DWORD,LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTextExt(LONG,LONG,RECT* rect,BOOL* clipped) override {
        if (!rect || !clipped) return E_POINTER; *rect={100,100,101,120}; *clipped=FALSE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(RECT* rect) override { if (!rect) return E_POINTER; *rect={100,100,900,600}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS* status) override { if (!status) return E_POINTER; *status={0,TS_SS_TRANSITORY|TS_SS_NOHIDDENTEXT}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWnd(HWND* window) override { if (!window) return E_POINTER; *window=nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAttribute(REFGUID,VARIANT* value) override { if (!value) return E_POINTER; VariantInit(value); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*,BOOL* allow) override { if (!allow) return E_POINTER; *allow=TRUE; ++started; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*,ITfRange*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override { ++ended; update(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext*,TfEditCookie,ITfEditRecord*) override { if (started>ended) update(); return S_OK; }
    HRESULT read(TfEditCookie cookie) {
        Microsoft::WRL::ComPtr<ITfRange> range,end;
        auto hr=context->GetStart(cookie,&range); if (FAILED(hr)) return hr;
        hr=context->GetEnd(cookie,&end); if (FAILED(hr)) return hr;
        range->ShiftEndToRange(cookie,end.Get(),TF_ANCHOR_END);
        wchar_t buffer[512]{}; ULONG count{};range->GetText(cookie,0,buffer,512,&count);
        Microsoft::WRL::ComPtr<ITfProperty> property;context->GetProperty(GUID_PROP_COMPOSING,&property);
        VARIANT value{};property->GetValue(cookie,range.Get(),&value);
        if (value.vt==VT_I4 && value.lVal) preview.assign(buffer,count);
        else { output.append(buffer,count);preview.clear();if(count)range->SetText(cookie,0,nullptr,0); }
        VariantClear(&value);return S_OK;
    }
};
