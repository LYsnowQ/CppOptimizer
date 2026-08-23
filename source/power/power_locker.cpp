#include "power/power_locker.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <utility>

namespace optimizer::power {

namespace {

// PowerLockType -> Windows POWER_REQUEST_TYPE。
POWER_REQUEST_TYPE ToWin32Type(PowerLockType type) noexcept {
    switch (type) {
        case PowerLockType::ExecutionRequired:
            return PowerRequestExecutionRequired;
        case PowerLockType::DisplayRequired:
            return PowerRequestDisplayRequired;
    }
    return PowerRequestExecutionRequired;
}

} // namespace

const wchar_t* PowerLockTypeToString(PowerLockType type) noexcept {
    switch (type) {
        case PowerLockType::ExecutionRequired:
            return L"execution";
        case PowerLockType::DisplayRequired:
            return L"display";
    }
    return L"unknown";
}

common::Result<PowerLockType> ParsePowerLockType(
    std::string_view name) noexcept {
    std::string folded;
    folded.reserve(name.size());
    for (const char ch : name) {
        folded.push_back(
            ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    if (folded == "execution") {
        return common::Result<PowerLockType>::Success(
            PowerLockType::ExecutionRequired);
    }
    if (folded == "display") {
        return common::Result<PowerLockType>::Success(
            PowerLockType::DisplayRequired);
    }
    return common::Result<PowerLockType>::Failure(common::Error::Validation(
        "ParsePowerLockType",
        L"unknown power lock type (expected execution/display)"));
}

common::Result<std::uint64_t> Win32PowerRequestBackend::CreateRequest(
    std::wstring_view reason) {
    // Windows 限制原因串最长 128 字符；截断到 127 防御（保留终止符空间）。
    // PowerCreateRequest 创建时复制原因串，调用返回后即可丢弃。
    constexpr std::size_t kMaxReasonChars = 127;
    std::wstring truncated(reason.substr(0, kMaxReasonChars));

    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = truncated.data();

    HANDLE handle = ::PowerCreateRequest(&context);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return common::Result<std::uint64_t>::Failure(
            common::Error::FromWin32(::GetLastError(), "PowerCreateRequest"));
    }
    return common::Result<std::uint64_t>::Success(
        reinterpret_cast<std::uint64_t>(handle));
}

common::Result<void> Win32PowerRequestBackend::SetRequest(
    std::uint64_t handle, PowerLockType type) {
    if (!::PowerSetRequest(reinterpret_cast<HANDLE>(handle),
                           ToWin32Type(type))) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "PowerSetRequest"));
    }
    return common::Result<void>::Success();
}

common::Result<void> Win32PowerRequestBackend::ClearRequest(
    std::uint64_t handle, PowerLockType type) {
    if (!::PowerClearRequest(reinterpret_cast<HANDLE>(handle),
                             ToWin32Type(type))) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "PowerClearRequest"));
    }
    return common::Result<void>::Success();
}

void Win32PowerRequestBackend::CloseRequest(std::uint64_t handle) noexcept {
    if (handle != 0) {
        // 尽力而为：关闭失败不抛出；进程退出时句柄表自动清理。
        (void)::CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
}

std::shared_ptr<PowerRequestBackend> CreateWin32Backend() noexcept {
    return std::make_shared<Win32PowerRequestBackend>();
}

PowerLocker::PowerLocker(std::shared_ptr<PowerRequestBackend> backend) noexcept
    : backend_(std::move(backend)) {}

PowerLocker::~PowerLocker() noexcept {
    ReleaseAll();
}

common::Result<void> PowerLocker::AcquireLock(PowerLockType type,
                                              std::wstring reason) {
    if (!backend_) {
        return common::Result<void>::Failure(common::Error::Validation(
            "PowerLocker::AcquireLock", L"no power request backend"));
    }
    LockState& state = states_[static_cast<std::size_t>(type)];
    if (state.count == 0) {
        // 0->1：真正创建并设置请求；失败不改动状态。
        auto created = backend_->CreateRequest(reason);
        if (!created) {
            return common::Result<void>::Failure(created.ErrorValue());
        }
        const std::uint64_t handle = created.Value();
        auto set = backend_->SetRequest(handle, type);
        if (!set) {
            backend_->CloseRequest(handle); // 不留悬空句柄
            return common::Result<void>::Failure(set.ErrorValue());
        }
        state.handle = handle;
        state.reason = std::move(reason);
    }
    ++state.count;
    return common::Result<void>::Success();
}

common::Result<void> PowerLocker::ReleaseLock(PowerLockType type) {
    LockState& state = states_[static_cast<std::size_t>(type)];
    if (state.count == 0) {
        return common::Result<void>::Success(); // 幂等：未持有为空操作
    }
    --state.count;
    if (state.count == 0) {
        // 1->0：清除并关闭请求。
        auto clear = backend_->ClearRequest(state.handle, type);
        if (!clear) {
            ++state.count; // 清除失败：请求仍生效，恢复计数以便调用方重试
            return common::Result<void>::Failure(clear.ErrorValue());
        }
        backend_->CloseRequest(state.handle);
        state.handle = 0;
        state.reason.clear();
    }
    return common::Result<void>::Success();
}

void PowerLocker::ReleaseAll() noexcept {
    for (std::size_t i = 0; i < states_.size(); ++i) {
        LockState& state = states_[i];
        if (state.count == 0) {
            continue;
        }
        if (state.handle != 0 && backend_) {
            // 尽力而为：ClearRequest 失败也继续关闭句柄；
            // CloseHandle 会取消该句柄上的全部请求（系统侧保证释放）。
            (void)backend_->ClearRequest(
                state.handle, static_cast<PowerLockType>(i));
            backend_->CloseRequest(state.handle);
        }
        state.handle = 0;
        state.reason.clear();
        state.count = 0;
    }
}

bool PowerLocker::IsLocked(PowerLockType type) const noexcept {
    return states_[static_cast<std::size_t>(type)].count > 0;
}

std::size_t PowerLocker::LockCount(PowerLockType type) const noexcept {
    return states_[static_cast<std::size_t>(type)].count;
}

const std::wstring& PowerLocker::LockReason(
    PowerLockType type) const noexcept {
    return states_[static_cast<std::size_t>(type)].reason;
}

} // namespace optimizer::power
