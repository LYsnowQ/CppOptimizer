#include "policy/policy_executor.hpp"

#include <utility>

namespace optimizer::policy {

PolicyExecutor::PolicyExecutor(
    std::shared_ptr<optimizer::power::PowerLocker> power,
    std::shared_ptr<optimizer::priority::PriorityBooster> priority,
    ExecutorConfig config) noexcept
    : power_(std::move(power)),
      priority_(std::move(priority)),
      config_(std::move(config)) {}

PolicyExecutor::~PolicyExecutor() noexcept {
    ReleaseAll();
}

common::Result<ExecutorEffect> PolicyExecutor::ApplyDecision(
    const PolicyDecision& decision, const ExecutorTarget& target) {
    ExecutorEffect effect;
    // IPC-017：R1 动作连续失败停摆（Safe Mode 类语义：只读咨询、不再触碰系统）。
    // 停摆期间不再调用后端；返回成功但附 skipped 说明（决策展示不受影响）。
    if (halted_) {
        effect.skipped =
            L"execution halted: " +
            std::to_wstring(config_.consecutiveActionFailuresToHalt) +
            L" consecutive R1 action failures (Safe Mode-like; advisory only)";
        return common::Result<ExecutorEffect>::Success(std::move(effect));
    }
    auto priorityResult = ReconcilePriority(decision, target, effect);
    if (!priorityResult) {
        // 失败不伪装成功；连续计数达阈值即停摆（R1 动作不再空转重试）。
        if (config_.consecutiveActionFailuresToHalt > 0) {
            ++consecutiveActionFailures_;
            if (consecutiveActionFailures_ >=
                config_.consecutiveActionFailuresToHalt) {
                halted_ = true;
            }
        }
        return common::Result<ExecutorEffect>::Failure(
            priorityResult.ErrorValue());
    }
    auto powerResult = ReconcilePower(target, effect);
    if (!powerResult) {
        if (config_.consecutiveActionFailuresToHalt > 0) {
            ++consecutiveActionFailures_;
            if (consecutiveActionFailures_ >=
                config_.consecutiveActionFailuresToHalt) {
                halted_ = true;
            }
        }
        return common::Result<ExecutorEffect>::Failure(
            powerResult.ErrorValue());
    }
    // 成功（含无需动作）：重置连续计数（“连续”语义——成功即断开失败序列）。
    consecutiveActionFailures_ = 0;
    return common::Result<ExecutorEffect>::Success(std::move(effect));
}

common::Result<void> PolicyExecutor::ReconcilePriority(
    const PolicyDecision& decision, const ExecutorTarget& target,
    ExecutorEffect& effect) {
    // 期望状态：决策建议提升 && 游戏运行 && 门禁开启。
    const bool desired = config_.priorityEnabled && target.running &&
                         decision.action == PolicyAction::SuggestPriorityBoost;
    if (desired) {
        if (target.pid == 0) {
            // 无有效进程身份（未观测到/已退出）：不执行，记录跳过原因。
            effect.skipped = L"priority boost skipped: no valid process identity";
            return common::Result<void>::Success();
        }
        // 已持有且身份一致：幂等，不重复获取。
        if (priorityGameId_ == target.gameId && priorityPid_ == target.pid) {
            return common::Result<void>::Success();
        }
        // 目标变化（重启/换游戏）：先释放旧动作。
        if (!priorityGameId_.empty()) {
            auto released =
                priority_->ReleaseBoost(priorityGameId_, priorityPid_);
            EmitAction("priority.unboost",
                       priorityGameId_ + " pid " +
                           std::to_string(priorityPid_),
                       "target change pre-release", released.HasValue());
            if (!released) {
                // 释放失败：提升仍生效，上报且保持记录，调用方可重试。
                return released;
            }
            effect.priorityReleased = true;
            priorityGameId_.clear();
        }
        auto acquired = priority_->AcquireBoost(
            target.gameId, target.pid, target.creationTime100ns,
            config_.priorityMaxLevel);
        EmitAction("priority.boost",
                   target.gameId + " pid " + std::to_string(target.pid),
                   "level=" +
                       std::to_string(
                           static_cast<int>(config_.priorityMaxLevel)),
                   acquired.HasValue());
        if (!acquired) {
            return acquired; // 失败不伪装成功
        }
        effect.priorityBoosted = true;
        priorityGameId_ = target.gameId;
        priorityPid_ = target.pid;
        return common::Result<void>::Success();
    }

    // 期望为假但当前持有：游戏退出或决策不再要求 -> 释放。
    if (!priorityGameId_.empty()) {
        auto released =
            priority_->ReleaseBoost(priorityGameId_, priorityPid_);
        EmitAction("priority.unboost",
                   priorityGameId_ + " pid " +
                       std::to_string(priorityPid_),
                   "game exit or no longer desired", released.HasValue());
        if (!released) {
            return released;
        }
        effect.priorityReleased = true;
        priorityGameId_.clear();
    }
    return common::Result<void>::Success();
}

common::Result<void> PolicyExecutor::ReconcilePower(
    const ExecutorTarget& target, ExecutorEffect& effect) {
    const bool desired = config_.powerExecutionRequired && target.running;
    if (desired && !powerHeld_) {
        auto locked = power_->AcquireLock(
            optimizer::power::PowerLockType::ExecutionRequired,
            config_.powerReason);
        // 审计详情用固定 ASCII 描述（不把宽字符 reason 窄化回显到审计记录）。
        EmitAction("power.hold", "policy (game running)",
                   "execution power request", locked.HasValue());
        if (!locked) {
            return locked; // 失败不伪装成功
        }
        powerHeld_ = true;
        effect.powerHeld = true;
        return common::Result<void>::Success();
    }
    if (!desired && powerHeld_) {
        auto released = power_->ReleaseLock(
            optimizer::power::PowerLockType::ExecutionRequired);
        EmitAction("power.release", "policy",
                   "game exit or no longer desired", released.HasValue());
        if (!released) {
            return released;
        }
        powerHeld_ = false;
        effect.powerReleased = true;
    }
    return common::Result<void>::Success();
}

void PolicyExecutor::ReleaseAll() noexcept {
    if (!priorityGameId_.empty() && priority_) {
        // 尽力而为：恢复失败不阻断（句柄随进程退出/析构清理）。
        auto released =
            priority_->ReleaseBoost(priorityGameId_, priorityPid_);
        EmitAction("priority.unboost",
                   priorityGameId_ + " pid " +
                       std::to_string(priorityPid_),
                   "release-all on exit", released.HasValue());
        priorityGameId_.clear();
    }
    if (powerHeld_ && power_) {
        auto released = power_->ReleaseLock(
            optimizer::power::PowerLockType::ExecutionRequired);
        EmitAction("power.release", "policy", "release-all on exit",
                   released.HasValue());
        powerHeld_ = false;
    }
}

bool PolicyExecutor::IsPriorityHeld() const noexcept {
    return !priorityGameId_.empty();
}

bool PolicyExecutor::IsPowerHeld() const noexcept {
    return powerHeld_;
}

bool PolicyExecutor::IsHalted() const noexcept {
    return halted_;
}

void PolicyExecutor::ResetHalt() noexcept {
    halted_ = false;
    consecutiveActionFailures_ = 0;
}

void PolicyExecutor::EmitAction(std::string operationId, std::string target,
                                std::string detail, bool ok) noexcept {
    if (!config_.actionObserver) {
        return;
    }
    ExecutorActionEvent event;
    event.operationId = std::move(operationId);
    event.target = std::move(target);
    event.detail = std::move(detail);
    event.ok = ok;
    try {
        config_.actionObserver(event); // 审计观察者异常不跨执行器传播
    } catch (...) {
    }
}

} // namespace optimizer::policy
