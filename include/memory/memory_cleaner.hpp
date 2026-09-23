#pragma once

#include "config/config_manager.hpp"

#include <vector>

namespace optimizer::memory {

// 内存清理的“步骤”种类（本切片只有计划，不执行）。
enum class CleanKind {
    WorkingSetTrim,  // 工作集修剪（轻量、局部可逆）
    StandbyListPurge // Standby/Modified List 清理（R2/R3：全局影响，需隔离环境验证）
};

// 步骤名（ASCII，恒成功）。
[[nodiscard]] const char* CleanKindToString(CleanKind kind) noexcept;

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

// 清理后端（可注入）：本切片只提供 **拒绝执行** 的真实后端与供测试使用的 fake；
// 真实系统调用（工作集修剪 / Standby 清理）属后续切片，且必须先满足门禁与隔离环境要求。
class CleanBackend {
public:
    virtual ~CleanBackend() = default;

    [[nodiscard]] virtual common::Result<void> Execute(CleanKind kind) = 0;
};

// 真实后端占位：**永远如实拒绝**（未实现真正的系统调用；不伪装成功）。
[[nodiscard]] CleanBackend& RefusingCleanBackend() noexcept;

} // namespace optimizer::memory
