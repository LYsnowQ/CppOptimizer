#pragma once

#include "config/config_manager.hpp"

#include <optional>
#include <vector>

namespace optimizer::memory {

// 内存清理的“步骤”种类。计划与编排在本模块；**真实后端**见 `working_set`（工作集修剪，
// 仅本进程）与各占位后端（其余步骤返回 Unsupported）。
enum class CleanKind {
    WorkingSetTrim,      // 工作集修剪（轻量、局部可逆）
    StandbyListPurge,    // Standby/Modified List 清理（R2/R3：全局影响，需隔离环境验证）
    SystemFileCacheTrim  // 系统文件缓存修剪（Deep 级；强度最高）
};

// 步骤名（ASCII，恒成功）。
[[nodiscard]] const char* CleanKindToString(CleanKind kind) noexcept;

// 步骤对应的**能力 ID**（冷却台账键 / 门禁表行名）。单一真相：CLI 与诊断表都从这里取，
// 避免“诊断显示可跑、执行却按另一个键判冷却”。
[[nodiscard]] const char* CleanKindCapabilityId(CleanKind kind) noexcept;

// 该步骤在本构建中是否有**真实后端**（当前仅 `WorkingSetTrim` 为真；S3/S4 仍是占位后端，
// 一律 `Unsupported`）。**纯函数，不尝试执行**。
[[nodiscard]] bool CleanStepImplemented(CleanKind kind) noexcept;

// 清理计划（纯函数结果）：`allowed` 仅在**级别合法且门禁全通过**时为真。
// `steps` 是“若门禁开放将要执行什么”，供 dry-run 如实展示；本模块不执行任何系统调用。
struct MemoryCleanPlan {
    bool allowed = false;
    bool levelWithinLimit = false; // 请求级别 <= 配置上限
    bool gatesAllowed = false;     // 六道门全通过
    std::vector<CleanKind> steps;
};

// 计划（纯函数）：
// - 请求为 `None` -> 无可执行内容（steps 空、allowed=false）；
// - 请求级别高于配置上限 -> `levelWithinLimit=false`（显式拒绝，不降级执行）；
// - 门禁未全通过 -> `gatesAllowed=false`（`steps` 仍给出“若开放将做什么”，便于诊断展示）。
[[nodiscard]] MemoryCleanPlan PlanMemoryClean(
    optimizer::config::CleanLevel requested,
    optimizer::config::CleanLevel configuredMax, bool gatesAllowed) noexcept;

// 清理后端（可注入）：真实实现见 `working_set`（工作集修剪，仅本进程）；
// 本文件另提供**保守后端**（拒绝执行）与供测试使用的 fake。
// 任何真实系统调用都必须先满足六道门禁（R3 步骤另需隔离环境确认与后端实现）。
class CleanBackend {
public:
    virtual ~CleanBackend() = default;

    [[nodiscard]] virtual common::Result<void> Execute(CleanKind kind) = 0;
};

// 保守后端：**永远如实拒绝**（不执行任何系统调用，也不伪装成功）。
[[nodiscard]] CleanBackend& RefusingCleanBackend() noexcept;

// 执行报告：只有在“确实尝试过至少一步”时才会返回（参 ExecuteMemoryCleanPlan 契约）。
struct CleanExecutionReport {
    std::size_t executed = 0;      // 已实际调用后端的步数
    std::size_t succeeded = 0;     // 其中成功的步数
    bool ok = false;               // 全部步骤均成功
    bool hasFailedStep = false;    // 是否在某一步失败（失败即停止）
    CleanKind failedStep{};        // 失败的步骤（hasFailedStep 为真时有效）
    std::wstring failureDetail;    // 后端失败信息（原样转写）
};

// 执行计划（编排，可注入后端）：
// - **计划未获许可或无可执行步骤 -> Failure 且不调用后端**（区分“没做”与“做了但失败”）；
// - 否则逐步调用后端，**失败即停**（不继续后续步骤），并返回带部分结果的报告。
[[nodiscard]] common::Result<CleanExecutionReport> ExecuteMemoryCleanPlan(
    const MemoryCleanPlan& plan, CleanBackend& backend) noexcept;

// 计划就绪度（纯函数）：真实执行前的**预检**。
// 理由：“失败即停” 能防“做一半”，但防不住“明知会失败还开始”——若计划里含有尚无真实后端的步骤，
// 就应在**任何系统调用之前**拒绝，而不是先修剪工作集再报 Standby 不可用。
struct CleanPlanReadiness {
    bool allImplemented = true;
    bool hasStep = false;
    std::optional<CleanKind> firstUnimplemented; // 第一个未实现的步骤（allImplemented=false 时有值）
};

[[nodiscard]] CleanPlanReadiness EvaluateCleanPlanReadiness(
    const MemoryCleanPlan& plan) noexcept;

} // namespace optimizer::memory
