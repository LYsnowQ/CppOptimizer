#include "activity/raw_input.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace optimizer::activity {

namespace {

// WM_INPUT（0x00FF）：原始输入消息。
constexpr UINT kRawInputMessage = 0x00FF;
// 通用桌面用途页下的鼠标/键盘 Usage。
constexpr USHORT kUsagePageGenericDesktop = 0x01;
constexpr USHORT kUsageMouse = 0x02;
constexpr USHORT kUsageKeyboard = 0x06;

// 构造注册设备表（Remove 为 true 时 dwFlags 加 RIDEV_REMOVE 用于注销）。
std::vector<RAWINPUTDEVICE> BuildDevices(
    void* hwnd, bool keyboard, bool mouse, bool remove) noexcept {
    std::vector<RAWINPUTDEVICE> devices;
    devices.reserve(2);
    const DWORD baseFlags = remove ? RIDEV_REMOVE : RIDEV_INPUTSINK;
    if (keyboard) {
        devices.push_back(RAWINPUTDEVICE{
            kUsagePageGenericDesktop, kUsageKeyboard, baseFlags, (HWND)hwnd});
    }
    if (mouse) {
        devices.push_back(RAWINPUTDEVICE{
            kUsagePageGenericDesktop, kUsageMouse, baseFlags, (HWND)hwnd});
    }
    return devices;
}

std::int64_t NowEpochMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

RawInputEventSource::RawInputEventSource(Options options)
    : options_(options) {}

common::Result<void> RawInputEventSource::Attach(void* hwnd) noexcept {
    if (hwnd == nullptr) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RawInputEventSource::Attach", L"hwnd 不能为空"));
    }
    bool expected = false;
    if (!attached_.compare_exchange_strong(expected, true)) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RawInputEventSource::Attach", L"已绑定窗口（先 Detach）"));
    }
    hwnd_.store(hwnd);
    auto devices = BuildDevices(hwnd, options_.keyboard, options_.mouse, false);
    if (!devices.empty() &&
        !::RegisterRawInputDevices(devices.data(),
                                   static_cast<UINT>(devices.size()),
                                   sizeof(RAWINPUTDEVICE))) {
        const DWORD code = ::GetLastError();
        hwnd_.store(nullptr);
        attached_.store(false);
        return common::Result<void>::Failure(
            common::Error::FromWin32(code, "RegisterRawInputDevices"));
    }
    // 绑定新窗口后重置事件时刻：上一窗口的输入时间线不再适用（不伪装旧输入）。
    lastEventEpochMs_.store(0);
    return common::Result<void>::Success();
}

void RawInputEventSource::Detach() noexcept {
    if (!attached_.load()) {
        return;
    }
    const void* hwnd = hwnd_.load();
    auto devices =
        BuildDevices(const_cast<void*>(hwnd), options_.keyboard, options_.mouse,
                     true);
    if (!devices.empty()) {
        // 注销尽力而为：失败（窗口已销毁等）不影响解除绑定。
        (void)::RegisterRawInputDevices(devices.data(),
                                        static_cast<UINT>(devices.size()),
                                        sizeof(RAWINPUTDEVICE));
    }
    hwnd_.store(nullptr);
    attached_.store(false);
    lastEventEpochMs_.store(0);
}

bool RawInputEventSource::IsAttached() const noexcept {
    return attached_.load();
}

bool RawInputEventSource::OnWindowMessage(std::uint32_t message,
                                          std::uintptr_t,
                                          std::uintptr_t) noexcept {
    if (message != kRawInputMessage) {
        return false;
    }
    const std::int64_t nowMs = NowEpochMs();
    lastEventEpochMs_.store(nowMs);
    eventCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::uint64_t RawInputEventSource::InputEventCount() const noexcept {
    return eventCount_.load(std::memory_order_relaxed);
}

std::optional<std::chrono::milliseconds>
RawInputEventSource::IdleDuration() const noexcept {
    const std::int64_t last = lastEventEpochMs_.load();
    if (last == 0) {
        return std::nullopt; // 尚无事件：不伪装
    }
    const std::int64_t nowMs = NowEpochMs();
    const std::int64_t elapsed = nowMs >= last ? nowMs - last : 0;
    return std::chrono::milliseconds(elapsed);
}

} // namespace optimizer::activity
