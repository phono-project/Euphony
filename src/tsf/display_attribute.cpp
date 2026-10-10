#include "display_attribute.h"
#include "guids.h"
#include <new>

namespace euphony::tsf {
namespace {
TF_DISPLAYATTRIBUTE default_attribute() {
    TF_DISPLAYATTRIBUTE result{};
    result.crText.type=TF_CT_NONE; result.crBk.type=TF_CT_NONE;
    result.lsStyle=TF_LS_SOLID; result.fBoldLine=FALSE;
    result.crLine.type=TF_CT_COLORREF; result.crLine.cr=RGB(69,93,178);
    result.bAttr=TF_ATTR_INPUT; return result;
}
class Attribute final : public ITfDisplayAttributeInfo {
    LONG refs_=1;
    TF_DISPLAYATTRIBUTE attribute_=default_attribute();
public:
    Attribute() { InterlockedIncrement(&objects); }
    ~Attribute() { InterlockedDecrement(&objects); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if (!result) return E_POINTER; *result=nullptr;
        if (iid!=IID_IUnknown && iid!=IID_ITfDisplayAttributeInfo) return E_NOINTERFACE;
        *result=static_cast<ITfDisplayAttributeInfo*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* result) override { if (!result) return E_POINTER; *result=kPreeditAttribute; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* result) override { if (!result) return E_POINTER; *result=SysAllocString(L"Euphony preedit"); return *result?S_OK:E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE* result) override { if (!result) return E_POINTER; *result=attribute_; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE* value) override { if (!value) return E_POINTER; attribute_=*value; return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { attribute_=default_attribute(); return S_OK; }
};
class Attributes final : public IEnumTfDisplayAttributeInfo {
    LONG refs_=1; bool used_{};
public:
    explicit Attributes(bool used=false) : used_(used) { InterlockedIncrement(&objects); }
    ~Attributes() { InterlockedDecrement(&objects); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if (!result) return E_POINTER; *result=nullptr;
        if (iid!=IID_IUnknown && iid!=IID_IEnumTfDisplayAttributeInfo) return E_NOINTERFACE;
        *result=static_cast<IEnumTfDisplayAttributeInfo*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo** result) override { if (!result) return E_POINTER; *result=new(std::nothrow) Attributes(used_); return *result?S_OK:E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count,ITfDisplayAttributeInfo** result,ULONG* fetched) override {
        if (!result || (!fetched && count!=1)) return E_POINTER;
        if (fetched) *fetched=0;
        if (!count) return S_OK;
        if (used_) return S_FALSE;
        auto* attribute=new(std::nothrow) Attribute;
        if (!attribute) return E_OUTOFMEMORY;
        result[0]=attribute; used_=true; if (fetched) *fetched=1; return count==1?S_OK:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { used_=false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override { if (!count) return S_OK; bool available=!used_; used_=true; return available&&count==1?S_OK:S_FALSE; }
};
}
HRESULT enum_attributes(IEnumTfDisplayAttributeInfo** result) { if (!result) return E_POINTER; *result=new(std::nothrow) Attributes; return *result?S_OK:E_OUTOFMEMORY; }
HRESULT get_attribute(REFGUID guid,ITfDisplayAttributeInfo** result) {
    if (!result) return E_POINTER; *result=nullptr;
    if (guid!=kPreeditAttribute) return E_INVALIDARG;
    *result=new(std::nothrow) Attribute; return *result?S_OK:E_OUTOFMEMORY;
}
}
