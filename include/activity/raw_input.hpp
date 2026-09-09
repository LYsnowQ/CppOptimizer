#pragma once

#include "common/error.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace optimizer::activity {

// 事件驱动输入观察源（Raw Input）：以 RIDEV_INPUTSINK 向宿主隐藏窗口注册键盘/鼠标原始输入，
// 收到 WM_INPUT 即记录一次“用户输入事件”——只记发生时刻，不读取输入内容
//（无 Hook、无内容采集；输入语义与 GetLastInputInfo 轮询互补：事件驱动用于有窗口/消息循环的
// 常驻形态，轮询仍作无窗口形态兜底）。
// 线程模型：OnWindowMessage 由拥有窗口的 UI 线程（消息循环）调用；事件计数与空闲时长可被
// 其它线程读取（原子，无锁）。
class RawInputEventSource {
public:
    struct Options {
        bool keyboard = true; // 注册键盘原始输入（usUsage 0x06）
        bool mouse = true;    // 注册鼠标原始输入（usUsage 0x02）
    };

    explicit RawInputEventSource(Options options = {});

    // 绑定窗口并注册原始输入（RegisterRawInputDevices，RIDEV_INPUTSINK：非前台也接收）。
    // 同一实例重复 Attach（未 Detach）或 hwnd 为空返回 Validation；注册失败返回 Win32 错误且
    // 不遗留绑定状态（可修正后重试）。Detach 后可再次 Attach。
    [[nodiscard]] common::Result<void> Attach(void* hwnd) noexcept;

    // 注销原始输入（RIDEV_REMOVE，尽力而为）并解除绑定。未绑定为空操作；事件计数保留。
    void Detach() noexcept;

    [[nodiscard]] bool IsAttached() const noexcept;

    // 在窗口过程默认处理前调用：message == WM_INPUT 时记录事件并返回 true（已处理），
    // 其余消息返回 false（不干预宿主窗口过程）。
    [[nodiscard]] bool OnWindowMessage(std::uint32_t message, std::uintptr_t wParam,
                                       std::uintptr_t lParam) noexcept;

    // 已记录的输入事件总数。
    [[nodiscard]] std::uint64_t InputEventCount() const noexcept;

    // 距最近一次输入事件的时长（steady_clock 毫秒）。尚无任何事件（未注册/刚绑定/确无输入）
    // 返回 nullopt（不伪装“刚刚有输入”）。
    [[nodiscard]] std::optional<std::chrono::milliseconds> IdleDuration() const noexcept;

private:
    Options options_;
    std::atomic<bool> attached_{false};
    std::atomic<void*> hwnd_{nullptr};
    std::atomic<std::uint64_t> eventCount_{0};
    // 最近事件时刻（steady_clock 纪元毫秒）；0 = 尚无事件。
    std::atomic<std::int64_t> lastEventEpochMs_{0};
};

} // namespace optimizer::activity
