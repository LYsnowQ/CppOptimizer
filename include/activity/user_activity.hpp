#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace optimizer::activity {

// 用户输入活动状态（MOD-ACT-001 首切片，R0 只读）。
// Unknown=查询失败/不可交互（不伪装成活跃/空闲），Active=最近有输入，Idle=超过空闲阈值。
enum class ActivityState { Unknown, Active, Idle };

// 状态名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* ActivityStateToString(ActivityState state) noexcept;

// 一次最近输入查询结果。nowTick/lastInputTick 均为 GetTickCount 语义：毫秒、32 位、可回绕
//（约 49.7 天）；从未输入时 lastInputTick 为系统启动至今的 tick（GetLastInputInfo 语义，
// 本模块只在“有输入后”做相对判定，绝对 0 不作为“从未输入”的可靠信号处理）。
struct InputActivitySample {
    std::uint32_t nowTick = 0;       // 当前时刻 tick
    std::uint32_t lastInputTick = 0; // 最近键鼠输入时刻 tick
};

// 最近输入查询后端（可注入 fake 确定性测试；真实实现见 CreateWin32LastInputBackend）。
// 只读、无 Hook、不采集输入内容。
class LastInputBackend {
public:
    virtual ~LastInputBackend() = default;

    // 查询当前时刻与最近输入时刻。查询失败（如非交互会话/服务会话 0）返回 Failure，
    // 调用方按 Unknown 降级，不伪装成 Active/Idle。
    [[nodiscard]] virtual common::Result<InputActivitySample> Query() = 0;
};

// Win32 后端：GetTickCount + GetLastInputInfo（kernel32/user32 只读，无新库）。
[[nodiscard]] std::shared_ptr<LastInputBackend> CreateWin32LastInputBackend();

// GetTickCount 语义时间差（毫秒，处理 32 位回绕）：假定真实间隔 < 2^31 ms（约 24.8 天）；
// toTick 早于 fromTick（时钟回退/异源）按 0 返回（不做负差）。纯函数。
[[nodiscard]] std::int64_t TickDeltaMs(std::uint32_t fromTick,
                                       std::uint32_t toTick) noexcept;

// 距最近输入的空闲毫秒（now - lastInput；同 TickDeltaMs(lastInputTick, nowTick)）。
[[nodiscard]] std::int64_t IdleMilliseconds(
    std::uint32_t nowTick, std::uint32_t lastInputTick) noexcept;

// 空闲分类：idleMs >= idleThresholdMs -> Idle，否则 Active。阈值非负由调用方保证
//（负阈值退化为恒 Active）。纯函数。
[[nodiscard]] ActivityState ClassifyActivity(
    std::uint32_t nowTick, std::uint32_t lastInputTick,
    std::int64_t idleThresholdMs) noexcept;

// 观测窗口汇总（样本计数按状态归类）。
struct ActivityWindowSummary {
    std::size_t samples = 0;
    std::size_t active = 0;
    std::size_t idle = 0;
    std::size_t unknown = 0;
};

// 前台有界观测：采样 sampleCount 次（间隔 sampleIntervalMs；为 0 时不等待，供确定性测试），
// 每次查询最近输入并分类；onSample(state, idleMs) 逐样本回调（CLI 打印等），查询失败样本回调
// Unknown 且计入 unknown（降级不伪装）。sampleCount == 0 -> Validation 拒绝。
[[nodiscard]] common::Result<ActivityWindowSummary> ObserveActivity(
    LastInputBackend& backend, std::size_t sampleCount,
    std::chrono::milliseconds sampleInterval, std::int64_t idleThresholdMs,
    const std::function<void(ActivityState, std::int64_t)>& onSample);

} // namespace optimizer::activity
