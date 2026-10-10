#pragma once
#include "win_util.h"
#include <nlohmann/json.hpp>
#include <functional>

namespace euphony {
constexpr std::uint32_t max_frame_size = 1024 * 1024;
bool ensure_broker();
class IpcClient {
public:
    explicit IpcClient(DWORD timeout_ms = 2000, bool auto_start = true) : timeout_ms_(timeout_ms), auto_start_(auto_start) {}
    nlohmann::json call(const nlohmann::json& request);
private:
    DWORD timeout_ms_;
    bool auto_start_;
};
// Framed UTF-8 JSON, bounded size and deadline; handles use FILE_FLAG_OVERLAPPED.
bool read_frame(HANDLE pipe, nlohmann::json& value, DWORD timeout_ms);
bool write_frame(HANDLE pipe, const nlohmann::json& value, DWORD timeout_ms);
class PipeSecurity {
public:
    PipeSecurity();
    ~PipeSecurity();
    SECURITY_ATTRIBUTES* get() { return &attributes_; }
private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};
}
