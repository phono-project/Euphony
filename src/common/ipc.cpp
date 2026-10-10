#include "ipc.h"
#include <sddl.h>
#include <algorithm>
#include <vector>

namespace euphony {
namespace {
bool transfer(HANDLE pipe, void* buffer, DWORD size, bool write, ULONGLONG deadline) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    DWORD offset = 0;
    while (offset < size) {
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD done = 0;
        BOOL result = write ? WriteFile(pipe, bytes + offset, size - offset, &done, &operation)
                            : ReadFile(pipe, bytes + offset, size - offset, &done, &operation);
        if (!result) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const auto now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(event.get(), static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                // The OVERLAPPED and its buffer must remain alive until cancellation completes.
                GetOverlappedResult(pipe, &operation, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &done, FALSE)) return false;
        }
        if (!done) return false;
        offset += done;
    }
    return true;
}
std::filesystem::path broker_path() {
    wchar_t override_path[32768]{};
    const auto count = GetEnvironmentVariableW(L"EUPHONY_BROKER_PATH", override_path, 32768);
    if (count && count < 32768) {
        const std::filesystem::path path(override_path);
        if (path.is_absolute() && std::filesystem::is_regular_file(path)) return path;
        throw std::runtime_error("EUPHONY_BROKER_PATH must name an existing absolute executable path");
    }
    const auto installed = installed_directory();
    if (!installed.empty() && std::filesystem::is_regular_file(installed / L"EuphonyBroker.exe"))
        return installed / L"EuphonyBroker.exe";
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ensure_broker), &module);
    auto directory = module_directory(module);
    auto path = directory / L"EuphonyBroker.exe";
    if (std::filesystem::exists(path)) return path;
    if (std::filesystem::exists(directory.parent_path() / L"EuphonyBroker.exe"))
        return directory.parent_path() / L"EuphonyBroker.exe";
    throw std::runtime_error("EuphonyBroker.exe not found");
}
}
bool read_frame(HANDLE pipe, nlohmann::json& value, DWORD timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    std::uint32_t size = 0;
    if (!transfer(pipe, &size, sizeof(size), false, deadline) || !size || size > max_frame_size) return false;
    std::string buffer(size, '\0');
    if (!transfer(pipe, buffer.data(), size, false, deadline)) return false;
    value = nlohmann::json::parse(buffer, nullptr, false);
    return !value.is_discarded() && value.is_object();
}
bool write_frame(HANDLE pipe, const nlohmann::json& value, DWORD timeout_ms) {
    std::string buffer = value.dump();
    if (buffer.empty() || buffer.size() > max_frame_size) return false;
    auto size = static_cast<std::uint32_t>(buffer.size());
    const auto deadline = GetTickCount64() + timeout_ms;
    return transfer(pipe, &size, sizeof(size), true, deadline) &&
        transfer(pipe, buffer.data(), size, true, deadline);
}
PipeSecurity::PipeSecurity() {
    // Only this user and SYSTEM. Low integrity desktop clients can connect;
    // remote clients are separately rejected by CreateNamedPipe.
    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + current_user_sid() + L")S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
        throw std::runtime_error("Cannot create pipe security descriptor");
    attributes_ = {sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
}
PipeSecurity::~PipeSecurity() { LocalFree(descriptor_); }
bool ensure_broker() {
    const auto name = pipe_name();
    if (WaitNamedPipeW(name.c_str(), 1)) return true;
    if (GetLastError() == ERROR_SEM_TIMEOUT) return true;
    const auto path = broker_path();
    std::wstring command = L"\"" + path.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, path.parent_path().c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
nlohmann::json IpcClient::call(const nlohmann::json& request) {
    const auto name = pipe_name();
    auto deadline = GetTickCount64() + timeout_ms_;
    UniqueHandle pipe;
    bool launched = false;
    do {
        pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe) break;
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) throw std::runtime_error("Cannot connect to Euphony broker");
        if (auto_start_ && !launched && error == ERROR_FILE_NOT_FOUND) {
            launched = ensure_broker();
            // Dictionary initialization on a cold disk can outlast a normal
            // request. This startup allowance stays on the client's worker.
            if (launched) deadline = GetTickCount64() + std::max<DWORD>(timeout_ms_, 15000);
        }
        WaitNamedPipeW(name.c_str(), 25);
        Sleep(5);
    } while (GetTickCount64() < deadline);
    if (!pipe) throw std::runtime_error("Euphony broker connection timed out");
    deadline = GetTickCount64() + timeout_ms_;
    auto remaining = [&]() -> DWORD {
        const auto now = GetTickCount64();
        return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    };
    nlohmann::json response;
    if (!write_frame(pipe.get(), request, remaining()) || !read_frame(pipe.get(), response, remaining()))
        throw std::runtime_error("Euphony broker request timed out or disconnected");
    // The broker must not disconnect until the response has been consumed:
    // DisconnectNamedPipe otherwise discards unread bytes from the pipe buffer.
    write_frame(pipe.get(), {{"ack", true}}, remaining());
    if (response.contains("ok") && response.at("ok") == false)
        throw std::runtime_error(response.value("error", "Euphony broker rejected the request"));
    return response;
}
}
