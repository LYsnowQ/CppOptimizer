#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace optimizer::scheduler {

// 单个线程的只读快照（R0）。
struct ThreadSnapshot {
    std::uint32_t threadId = 0;
    int priority = 0;             // Win32 线程优先级（-15..15，0=normal）
    std::uint64_t affinityMask = 0; // 允许的 CPU 掩码（0 视为未知/未设置）
};

// 可注入后端：真实实现走 Toolhelp + OpenThread；测试用 fake。
// 任何失败都如实返回 Failure，不伪造读数。
class SchedulerBackend {
public:
    virtual ~SchedulerBackend() = default;

    [[nodiscard]] virtual common::Result<std::vector<ThreadSnapshot>> ListThreads(
        std::uint32_t processId) = 0;

    [[nodiscard]] virtual common::Result<int> GetPriority(
        std::uint32_t threadId) = 0;

    [[nodiscard]] virtual common::Result<void> SetPriority(
        std::uint32_t threadId, int priority) = 0;

    [[nodiscard]] virtual common::Result<std::uint64_t> GetAffinity(
        std::uint32_t threadId) = 0;

    [[nodiscard]] virtual common::Result<void> SetAffinity(
        std::uint32_t threadId, std::uint64_t mask) = 0;
};

// 真实后端（Toolhelp 枚举 + OpenThread；只在需要时请求 SET_INFORMATION）。
[[nodiscard]] SchedulerBackend& RealSchedulerBackend() noexcept;

// 纯函数：是否应把线程优先级提升到 target（只在 current < target 时为真；
// current >= target 一律不动，避免把线程降级）。
[[nodiscard]] bool ShouldRaisePriority(int current, int target) noexcept;

// 优先级范围校验（Win32 合法区间 -15..15）。
[[nodiscard]] bool IsValidPriority(int priority) noexcept;

// 一次提升的结果：保存原值，供条件恢复使用。
struct PriorityAdjustment {
    std::uint32_t threadId = 0;
    int originalPriority = 0;
    int appliedPriority = 0;
};

struct BoostReport {
    std::vector<PriorityAdjustment> adjusted; // 真正改动过的线程
    std::size_t skipped = 0;                  // 已 ≥ target 或读取失败的线程
};

// 对进程内所有优先级低于 target 的线程提升到 target（单次，不循环）。
// - 先逐线程读原值，再写；写失败立即停止并返回已完成的调整（不伪装全部成功）；
// - 调用方负责把这些调整持久化，以便之后 RestorePriorities。
[[nodiscard]] common::Result<BoostReport> BoostProcessThreads(
    SchedulerBackend& backend, std::uint32_t processId, int targetPriority);

// 条件恢复：仅当线程当前值仍等于 appliedPriority 时才写回原值
// （外部已改过的线程不动，避免覆盖他人修改）。返回实际恢复的线程数。
[[nodiscard]] common::Result<std::size_t> RestorePriorities(
    SchedulerBackend& backend, const std::vector<PriorityAdjustment>& adjustments);

// 调整记录的序列化（每用户小文件，信封 + 逐行），用于进程退出后恢复。
[[nodiscard]] std::string FormatAdjustmentRecord(
    std::uint32_t processId, const std::vector<PriorityAdjustment>& adjustments);

[[nodiscard]] common::Result<std::vector<PriorityAdjustment>> ParseAdjustmentRecord(
    std::string_view text);

} // namespace optimizer::scheduler
