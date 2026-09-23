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
    // 级别 -> 步骤（Light = 工作集修剪；Standby 清理属更高级别，当前枚举内不可达）。
    plan.steps.push_back(CleanKind::WorkingSetTrim);
    plan.allowed = gatesAllowed;
    return plan;
}

CleanBackend& RefusingCleanBackend() noexcept {
    static RefusingCleanBackendImpl backend;
    return backend;
}

} // namespace optimizer::memory
