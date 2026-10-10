#pragma once
#include <msctf.h>
#include <textstor.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <string>

// A small real TSF document host for integration tests. It has no visible
// editor and explicitly requests UIless candidate presentation.
class TestTextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink, public ITfUIElementSink {
    LONG refs_=1;
    Microsoft::WRL::ComPtr<ITextStoreACPSink> sink_;
    DWORD lock_{};
    TS_SELECTION_ACP selection_{0,0,{TS_AE_END,FALSE}};
    HWND window_{};
public:
    std::wstring text;
    UINT started{},ended{},ui_updates{};
    bool show_candidates = false, defer_layout = false;
    int layout_state = 0; // 0 ready, 1 TS_E_NOLAYOUT, 2 successful empty extent.
    void publish_layout(int state) { layout_state=state; if (sink_) sink_->OnLayoutChange(TS_LC_CHANGE,1); }
    TestTextStore() { window_=CreateWindowExW(0,L"EDIT",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr); }
    ~TestTextStore() { if (window_) DestroyWindow(window_); }
    void password(bool value) { SetWindowLongPtrW(window_,GWL_STYLE,WS_POPUP|(value?ES_PASSWORD:0)); }
    void clear() {
        TS_TEXTCHANGE change{0,static_cast<LONG>(text.size()),0}; text.clear(); selection_.acpStart=selection_.acpEnd=0;
        if (sink_) { sink_->OnTextChange(0,&change); sink_->OnSelectionChange(); }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if (!out) return E_POINTER; *out=nullptr;
        if (iid==IID_IUnknown || iid==IID_ITextStoreACP) *out=static_cast<ITextStoreACP*>(this);
        else if (iid==IID_ITfContextOwnerCompositionSink) *out=static_cast<ITfContextOwnerCompositionSink*>(this);
        else if (iid==IID_ITfUIElementSink) *out=static_cast<ITfUIElementSink*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid,IUnknown* object,DWORD) override {
        if (iid!=IID_ITextStoreACPSink || !object) return E_INVALIDARG;
        sink_.Reset(); return object->QueryInterface(IID_PPV_ARGS(&sink_));
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown*) override { sink_.Reset(); return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags,HRESULT* result) override {
        if (!result || !sink_) return E_INVALIDARG;
        if (lock_) { *result=TS_E_SYNCHRONOUS; return S_OK; }
        lock_=flags; *result=sink_->OnLockGranted(flags); lock_=0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override { if (!status) return E_POINTER; *status={0,TS_SS_NOHIDDENTEXT}; return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryInsert(LONG start,LONG end,ULONG,LONG* first,LONG* last) override {
        if (!first || !last || start<0 || end<start || end>static_cast<LONG>(text.size())) return E_INVALIDARG;
        *first=start; *last=end; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(ULONG index,ULONG count,TS_SELECTION_ACP* selection,ULONG* fetched) override {
        if (!selection || !fetched) return E_POINTER; *fetched=0;
        if ((index!=TS_DEFAULT_SELECTION && index!=0) || !count) return E_INVALIDARG;
        *selection=selection_; *fetched=1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(ULONG count,const TS_SELECTION_ACP* selection) override {
        if (count!=1 || !selection || selection->acpStart<0 || selection->acpEnd<selection->acpStart || selection->acpEnd>static_cast<LONG>(text.size())) return E_INVALIDARG;
        selection_=*selection; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(LONG start,LONG end,WCHAR* plain,ULONG requested,ULONG* returned,TS_RUNINFO* runs,ULONG run_requested,ULONG* run_returned,LONG* next) override {
        if (!returned || !run_returned || !next) return E_POINTER;
        if (end==-1) end=static_cast<LONG>(text.size());
        if (start<0 || end<start || end>static_cast<LONG>(text.size())) return TS_E_INVALIDPOS;
        auto plain_amount=std::min<ULONG>(requested,static_cast<ULONG>(end-start));
        const auto amount=requested?plain_amount:(run_requested?static_cast<ULONG>(end-start):0);
        if (plain_amount && !plain) return E_POINTER;
        if (plain_amount) std::memcpy(plain,text.data()+start,plain_amount*sizeof(wchar_t));
        *returned=plain_amount; *next=start+amount; *run_returned=0;
        if (amount && run_requested && runs) { runs[0]={amount,TS_RT_PLAIN}; *run_returned=1; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetText(DWORD,LONG start,LONG end,const WCHAR* value,ULONG count,TS_TEXTCHANGE* change) override {
        if (!change || start<0 || end<start || end>static_cast<LONG>(text.size()) || (count && !value)) return E_INVALIDARG;
        text.replace(static_cast<std::size_t>(start),static_cast<std::size_t>(end-start),value?value:L"",count);
        if (defer_layout) layout_state=1;
        *change={start,end,start+static_cast<LONG>(count)};
        selection_.acpStart=selection_.acpEnd=change->acpNewEnd; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG,LONG,IDataObject** out) override { if (out) *out=nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG,REFGUID,REFIID,IUnknown** out) override { if (out) *out=nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*,const FORMATETC*,BOOL* out) override { if (!out) return E_POINTER; *out=FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD,LONG,LONG,IDataObject*,TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags,const WCHAR* value,ULONG count,LONG* start,LONG* end,TS_TEXTCHANGE* change) override {
        if (start) *start=selection_.acpStart; if (end) *end=selection_.acpEnd;
        if (flags&TS_IAS_QUERYONLY) return S_OK;
        TS_TEXTCHANGE ignored{}; return SetText(0,selection_.acpStart,selection_.acpEnd,value,count,change?change:&ignored);
    }
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD,IDataObject*,LONG*,LONG*,TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD,ULONG,const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG,ULONG,const TS_ATTRID*,DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG,ULONG,const TS_ATTRID*,DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG,LONG stop,ULONG,const TS_ATTRID*,DWORD,LONG* next,BOOL* found,LONG* offset) override {
        if (!next || !found || !offset) return E_POINTER; *next=stop; *found=FALSE; *offset=0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG,TS_ATTRVAL*,ULONG* fetched) override { if (!fetched) return E_POINTER; *fetched=0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEndACP(LONG* end) override { if (!end) return E_POINTER; *end=static_cast<LONG>(text.size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override { if (!view) return E_POINTER; *view=1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie,const POINT*,DWORD,LONG* acp) override { if (!acp) return E_POINTER; *acp=selection_.acpEnd; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie,LONG start,LONG end,RECT* rect,BOOL* clipped) override {
        if (rect && clipped && layout_state) { *rect={}; *clipped=FALSE; return layout_state==1?TS_E_NOLAYOUT:S_OK; }
        if (!rect || !clipped) return E_POINTER; *rect={100+start*10,100,100+end*10+1,120}; *clipped=FALSE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie,RECT* rect) override { if (!rect) return E_POINTER; *rect={100,100,900,600}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie,HWND* window) override { if (!window) return E_POINTER; *window=window_; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*,BOOL* allow) override { if (!allow) return E_POINTER; *allow=TRUE; ++started; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*,ITfRange*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override { ++ended; return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD,BOOL* show) override { if (!show) return E_POINTER; *show=show_candidates?TRUE:FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD) override { ++ui_updates; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD) override { return S_OK; }
};
