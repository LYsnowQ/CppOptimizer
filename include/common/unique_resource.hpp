#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <winsvc.h>

#include <utility>

namespace optimizer::common {

// Owns handles returned by Create*/Open* APIs whose documented closer is CloseHandle.
// GetCurrentProcess/GetCurrentThread pseudo handles and borrowed handles must not be adopted.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() noexcept { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.Release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const noexcept { return IsValid(); }

    HANDLE Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void Reset(HANDLE newHandle = nullptr) noexcept {
        if (IsValid()) {
            ::CloseHandle(handle_);
        }
        handle_ = newHandle;
    }

private:
    HANDLE handle_ = nullptr;
};

// SC_HANDLE has a distinct release function and must never be passed to CloseHandle.
class UniqueServiceHandle {
public:
    UniqueServiceHandle() noexcept = default;
    explicit UniqueServiceHandle(SC_HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueServiceHandle() noexcept { Reset(); }

    UniqueServiceHandle(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;

    UniqueServiceHandle(UniqueServiceHandle&& other) noexcept
        : handle_(other.Release()) {}

    UniqueServiceHandle& operator=(UniqueServiceHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] SC_HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return IsValid(); }

    SC_HANDLE Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void Reset(SC_HANDLE newHandle = nullptr) noexcept {
        if (handle_ != nullptr) {
            ::CloseServiceHandle(handle_);
        }
        handle_ = newHandle;
    }

private:
    SC_HANDLE handle_ = nullptr;
};

struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept {
        if (memory != nullptr) {
            ::LocalFree(memory);
        }
    }
};

struct CoTaskMemFreeDeleter {
    void operator()(void* memory) const noexcept {
        if (memory != nullptr) {
            ::CoTaskMemFree(memory);
        }
    }
};

} // namespace optimizer::common
