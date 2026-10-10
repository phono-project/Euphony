#include "guids.h"
#include <msctf.h>
#include <string>
#include <new>
#include <vector>

namespace euphony::tsf {
HMODULE module = nullptr;
LONG objects = 0;
namespace {
class Factory final : public IClassFactory {
    LONG refs_ = 1;
public:
    Factory() { InterlockedIncrement(&objects); }
    ~Factory() { InterlockedDecrement(&objects); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n = InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        return create_service(iid, out);
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) InterlockedIncrement(&objects); else InterlockedDecrement(&objects); return S_OK; }
};
std::wstring clsid_path() {
    wchar_t id[40]{}; StringFromGUID2(kService, id, 40);
    return std::wstring(L"Software\\Classes\\CLSID\\") + id;
}
std::wstring tip_path() {
    wchar_t id[40]{}; StringFromGUID2(kService, id, 40);
    return std::wstring(L"Software\\Microsoft\\CTF\\TIP\\") + id;
}
HRESULT registry_string(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key{};
    // COM is registered in the current process's machine registry view. TSF's
    // profile registration is machine-wide, while profile enablement is per user.
    auto err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    err = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key); return HRESULT_FROM_WIN32(err);
}
const GUID* categories[] = {&GUID_TFCAT_TIP_KEYBOARD, &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER};
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { euphony::tsf::module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return euphony::tsf::objects == 0 ? S_OK : S_FALSE; }
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    using namespace euphony::tsf;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != kService) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new(std::nothrow) Factory;
    if (!factory) return E_OUTOFMEMORY;
    auto hr = factory->QueryInterface(iid, out); factory->Release(); return hr;
}
extern "C" HRESULT __stdcall DllUnregisterServer() {
    using namespace euphony::tsf;
    HRESULT result = S_OK;
    auto remember = [&](HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result)) result = hr; };
    auto remember_registry = [&](LSTATUS error) {
        if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            remember(HRESULT_FROM_WIN32(error));
    };
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) remember(init);
    HKEY tip{};
    const auto present = RegOpenKeyExW(HKEY_LOCAL_MACHINE, tip_path().c_str(), 0, KEY_READ, &tip);
    if (tip) RegCloseKey(tip);
    remember_registry(present);
    // The shared TSF registration can already have been removed by the other
    // architecture. Do not turn an absent profile into an unspecified E_FAIL.
    if (present != ERROR_FILE_NOT_FOUND && present != ERROR_PATH_NOT_FOUND &&
        (SUCCEEDED(init) || init == RPC_E_CHANGED_MODE)) {
        ITfCategoryMgr* mgr{};
        auto hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr));
        remember(hr);
        if (mgr) {
            IEnumGUID* list{};
            hr = mgr->EnumCategoriesInItem(kService, &list); remember(hr);
            std::vector<GUID> registered;
            if (list) {
                GUID category{}; ULONG fetched{};
                while ((hr = list->Next(1, &category, &fetched)) == S_OK && fetched)
                    registered.push_back(category);
                remember(hr); list->Release();
            }
            for (auto category : categories) {
                for (const auto& found : registered) {
                    if (found == *category) { remember(mgr->UnregisterCategory(kService, *category, kService)); break; }
                }
            }
            mgr->Release();
        }
        ITfInputProcessorProfiles* profiles{};
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles));
        remember(hr);
        if (profiles) { remember(profiles->Unregister(kService)); profiles->Release(); }
    }
    remember_registry(RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_path().c_str()));
    // Remove a legacy per-user COM override left by earlier development builds.
    remember_registry(RegDeleteTreeW(HKEY_CURRENT_USER, clsid_path().c_str()));
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}
extern "C" HRESULT __stdcall DllRegisterServer() {
    using namespace euphony::tsf;
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return HRESULT_FROM_WIN32(GetLastError());
    auto hr = registry_string(clsid_path(), nullptr, L"Euphony");
    if (FAILED(hr)) return hr;
    hr = registry_string(clsid_path()+L"\\InprocServer32", nullptr, path);
    if (SUCCEEDED(hr)) hr = registry_string(clsid_path()+L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    if (FAILED(hr)) { DllUnregisterServer(); return hr; }
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) { DllUnregisterServer(); return init; }
    ITfInputProcessorProfiles* profiles{};
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles));
    if (SUCCEEDED(hr)) {
        hr = profiles->Register(kService);
        constexpr wchar_t name[] = L"Euphony";
        if (SUCCEEDED(hr)) hr = profiles->AddLanguageProfile(kService, kLanguage, kProfile, name, 7, path, length, 0);
        if (SUCCEEDED(hr)) hr = profiles->EnableLanguageProfile(kService, kLanguage, kProfile, TRUE);
        profiles->Release();
    }
    ITfCategoryMgr* mgr{};
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr));
    if (mgr) { for (auto category : categories) { if (SUCCEEDED(hr)) hr = mgr->RegisterCategory(kService, *category, kService); } mgr->Release(); }
    if (SUCCEEDED(init)) CoUninitialize();
    if (SUCCEEDED(hr)) {
        // A per-user override from a development build otherwise shadows the
        // machine COM registration in the application's merged HKCR view.
        const auto legacy = RegDeleteTreeW(HKEY_CURRENT_USER, clsid_path().c_str());
        if (legacy != ERROR_SUCCESS && legacy != ERROR_FILE_NOT_FOUND && legacy != ERROR_PATH_NOT_FOUND)
            hr = HRESULT_FROM_WIN32(legacy);
    }
    if (FAILED(hr)) DllUnregisterServer();
    return hr;
}
