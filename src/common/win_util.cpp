#include "win_util.h"
#include <sddl.h>
#include <shlobj.h>
#include <vector>

namespace euphony {
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid UTF-16");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::filesystem::path module_directory(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const auto count = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) throw std::runtime_error("Cannot locate module");
    buffer.resize(count);
    return std::filesystem::path(buffer).parent_path();
}
std::filesystem::path user_directory() {
    wchar_t override_path[32768]{};
    const auto size = GetEnvironmentVariableW(L"EUPHONY_USER_DIR", override_path, 32768);
    if (size && size < 32768) return std::filesystem::path(override_path);
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        throw std::runtime_error("Cannot locate LocalAppData");
    const auto result = std::filesystem::path(path) / L"Euphony";
    CoTaskMemFree(path);
    return result;
}
std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot open user token");
    UniqueHandle holder(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) throw std::runtime_error("Cannot read user token");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
        throw std::runtime_error("Cannot format user SID");
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}
std::filesystem::path installed_directory() {
    // Explicit isolated test instances resolve their own binaries, never the daily install.
    if (GetEnvironmentVariableW(L"EUPHONY_INSTANCE",nullptr,0)>1) return {};
    wchar_t path[32768]{}; DWORD bytes=sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER,L"Software\\Euphony",L"InstallDir",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,
        nullptr,path,&bytes)==ERROR_SUCCESS && path[0]) return std::filesystem::path(path);
    return {};
}
std::wstring instance_name() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    std::wstring result = current_user_sid() + L"-" + std::to_wstring(session);
    // An explicit suffix isolates development/test brokers from the installed IME.
    wchar_t suffix[65]{};
    const auto count = GetEnvironmentVariableW(L"EUPHONY_INSTANCE", suffix, 65);
    if (count && count < 65) {
        for (DWORD i = 0; i < count; ++i)
            if (!(iswalnum(suffix[i]) || suffix[i] == L'-' || suffix[i] == L'_'))
                throw std::runtime_error("Invalid EUPHONY_INSTANCE");
        result += L"-" + std::wstring(suffix);
    }
    return result;
}
std::wstring pipe_name() { return L"\\\\.\\pipe\\Euphony-v1-" + instance_name(); }
}
