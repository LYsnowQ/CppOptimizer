#include "memory/memory_cleaner.hpp"

#include "common/error.hpp"

namespace optimizer::memory {

namespace {

class RefusingCleanBackendImpl final : public CleanBackend {
public:
    [[nodiscard]] common::Result<void> Execute(CleanKind kind) override {
        (void)kind;
        return common::Result<void>::Failure(common::Error::Unsupported(
            "CleanBackend::Execute",
            L"内存清理未实现：真实系统调用需先满足六道门禁并在隔离环境验证"));
    }
};

} // namespace

const char* CleanKindToString(CleanKind kind) noexcept {
    switch (kind) {
        case CleanKind::WorkingSetTrim:
            return "working_set_trim";
        case CleanKind::StandbyListPurge:
            return "standby_list_purge";
        case CleanKind::SystemFileCacheTrim:
            return "system_file_cache_trim";
    }
    return "unknown";
}

MemoryCleanPlan PlanMemoryClean(optimizer::config::CleanLevel requested,
                                optimizer::config::CleanLevel configuredMax,
                                bool gatesAllowed) noexcept {
    MemoryCleanPlan plan;
    plan.gatesAllowed = gatesAllowed;
    if (requested == optimizer::config::CleanLevel::None) {
        // 未请求清理：无可执行内容（不是“被拒绝”）。
        plan.levelWithinLimit = true;
        return plan;
    }
    plan.levelWithinLimit = requested <= configuredMax;
    if (!plan.levelWithinLimit) {
        return plan; // 超上限：显式拒绝，不降级执行
    }
    // 级别 -> 步骤（**递进**：Light ⊂ Medium ⊂ Deep；顺序 = 由轻到重，执行时失败即停）。
    const int level = static_cast<int>(requested);
    if (level >= static_cast<int>(optimizer::config::CleanLevel::Light)) {
        plan.steps.push_back(CleanKind::WorkingSetTrim);
    }
    if (level >= static_cast<int>(optimizer::config::CleanLevel::Medium)) {
        plan.steps.push_back(CleanKind::StandbyListPurge);
    }
    if (level >= static_cast<int>(optimizer::config::CleanLevel::Deep)) {
        plan.steps.push_back(CleanKind::SystemFileCacheTrim);
    }
    plan.allowed = gatesAllowed;
    return plan;
}

CleanBackend& RefusingCleanBackend() noexcept {
    static RefusingCleanBackendImpl backend;
    return backend;
}

common::Result<CleanExecutionReport> ExecuteMemoryCleanPlan(
    const MemoryCleanPlan& plan, CleanBackend& backend) noexcept {
    if (!plan.allowed) {
        // 未获许可：一步也不做（不是“执行失败”，而是“压根没开始”）。
        return common::Result<CleanExecutionReport>::Failure(
            common::Error::Unsupported(
                "ExecuteMemoryCleanPlan",
                L"计划未获门禁许可：不执行任何步骤"));
    }
    if (plan.steps.empty()) {
        return common::Result<CleanExecutionReport>::Failure(
            common::Error::Validation("ExecuteMemoryCleanPlan",
                                      L"计划没有可执行步骤"));
    }
    CleanExecutionReport report;
    for (const auto step : plan.steps) {
        ++report.executed;
        const auto result = backend.Execute(step);
        if (!result) {
            report.hasFailedStep = true;
            report.failedStep = step;
            report.failureDetail = result.ErrorValue().message;
            return common::Result<CleanExecutionReport>::Success(
                std::move(report)); // 失败即停，但如实携带部分结果
        }
        ++report.succeeded;
    }
    report.ok = true;
    return common::Result<CleanExecutionReport>::Success(std::move(report));
}

} // namespace optimizer::memory
