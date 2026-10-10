#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <stdexcept>

namespace euphony {
std::wstring wide(std::string_view text);
std::string utf8(std::wstring_view text);
std::filesystem::path module_directory(HMODULE module = nullptr);
std::filesystem::path installed_directory();
std::filesystem::path user_directory();
std::wstring current_user_sid();
std::wstring pipe_name();
std::wstring instance_name();
class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    HANDLE get() const { return value_; }
    HANDLE release() { auto result = value_; value_ = nullptr; return result; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
}
