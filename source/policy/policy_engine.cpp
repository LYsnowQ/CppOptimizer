#include "policy/policy_engine.hpp"

namespace optimizer::policy {

const wchar_t* PressureToString(ResourcePressure pressure) noexcept {
    switch (pressure) {
        case ResourcePressure::Comfortable:
            return L"comfortable";
        case ResourcePressure::Adequate:
            return L"adequate";
        case ResourcePressure::Tight:
            return L"tight";
        case ResourcePressure::Critical:
            return L"critical";
    }
    return L"unknown";
}

const wchar_t* ActionToString(PolicyAction action) noexcept {
    switch (action) {
        case PolicyAction::NoOp:
            return L"NoOp";
        case PolicyAction::Notify:
            return L"Notify";
        case PolicyAction::SuggestMemoryTune:
            return L"SuggestMemoryTune";
        case PolicyAction::SuggestPriorityBoost:
            return L"SuggestPriorityBoost";
    }
    return L"Unknown";
}

common::Result<std::uint32_t> ComputeMemoryMarginPercent(
    std::uint64_t availableBytes, std::uint64_t totalBytes) noexcept {
    if (totalBytes == 0 || availableBytes > totalBytes) {
        return common::Result<std::uint32_t>::Failure(
            common::Error::Validation(
                "ComputeMemoryMarginPercent",
                L"invalid memory counts (total == 0 or available > total)"));
    }
    // 64 位乘防溢出：available * 100 <= total * 100，total < 2^64 / 100 时仍安全；
    // 对 Windows 物理内存规模（< 2^40）恒安全。
    return common::Result<std::uint32_t>::Success(
        static_cast<std::uint32_t>((availableBytes * 100) / totalBytes));
}

common::Result<ResourcePressure> ClassifyPressure(
    std::int32_t marginPercent,
    const PolicyThresholds& thresholds) noexcept {
    if (marginPercent < 0 || marginPercent > 100) {
        return common::Result<ResourcePressure>::Failure(
            common::Error::Validation(
                "ClassifyPressure",
                L"margin percent must be in 0..100"));
    }
    if (!(thresholds.tightMarginPercent >= 0 &&
          thresholds.tightMarginPercent < thresholds.adequateMarginPercent &&
          thresholds.adequateMarginPercent < thresholds.comfortableMarginPercent &&
          thresholds.comfortableMarginPercent <= 100)) {
        return common::Result<ResourcePressure>::Failure(
            common::Error::Validation(
                "ClassifyPressure",
                L"thresholds must satisfy 0 <= tight < adequate < comfortable <= 100"));
    }
    if (marginPercent > thresholds.comfortableMarginPercent) {
        return common::Result<ResourcePressure>::Success(
            ResourcePressure::Comfortable);
    }
    if (marginPercent > thresholds.adequateMarginPercent) {
        return common::Result<ResourcePressure>::Success(
            ResourcePressure::Adequate);
    }
    if (marginPercent > thresholds.tightMarginPercent) {
        return common::Result<ResourcePressure>::Success(
            ResourcePressure::Tight);
    }
    return common::Result<ResourcePressure>::Success(
        ResourcePressure::Critical);
}

PolicyDecision EvaluatePolicy(const PolicyInput& input) noexcept {
    PolicyDecision decision;
    decision.gameId = input.game.gameId;

    // 规则 1：无游戏运行 -> 不产生任何建议。
    if (!input.game.running || input.game.gameId.empty()) {
        decision.action = PolicyAction::NoOp;
        decision.reasonCode = "no_game";
        decision.reason = "无游戏在运行，不产生优化建议";
        return decision;
    }

    // 规则 2：压力危急 -> 仅提示，不执行（红线保护：危急时刻撤销实验动作）。
    if (input.pressure == ResourcePressure::Critical) {
        decision.action = PolicyAction::Notify;
        decision.reasonCode = "mem_critical";
        decision.reason = "内存余量危急（余量 <= 紧张阈值），仅提示不执行任何动作";
        return decision;
    }

    // 规则 3：游戏后台且配置后台暂停 -> 不产生建议。
    if (!input.game.foreground && input.game.pauseWhenBackground) {
        decision.action = PolicyAction::NoOp;
        decision.reasonCode = "game_background";
        decision.reason = "游戏在后台且配置后台暂停优化，不产生建议";
        return decision;
    }

    // 规则 4：压力紧张 -> 建议 Layer 2 内存维护（无 R2 执行器，仅咨询）。
    if (input.pressure == ResourcePressure::Tight) {
        decision.action = PolicyAction::SuggestMemoryTune;
        decision.reasonCode = "mem_tight";
        decision.reason = "内存余量紧张，建议 Layer 2 内存维护（咨询，无执行器）";
        return decision;
    }

    // 规则 5（PWR-002）：压力 Adequate/Comfortable 且游戏在前台
    // -> 建议 Layer 3 优先级提升（由 PolicyExecutor 经 PriorityBooster 落地）。
    // 仅前台提升；后台游戏（含未暂停）不提升，避免与可见应用抢调度。
    if (input.game.foreground) {
        decision.action = PolicyAction::SuggestPriorityBoost;
        decision.reasonCode = "prio_boost";
        decision.reason = "游戏前台且内存余量充足，建议 Layer 3 优先级提升";
        return decision;
    }

    // 规则 6：其余（后台未暂停 + 余量充足）-> 无证据不优化。
    decision.action = PolicyAction::NoOp;
    decision.reasonCode = "mem_ok";
    decision.reason = "资源余量充足或一般，无证据不启用优化";
    return decision;
}

HysteresisFilter::HysteresisFilter(
    std::chrono::milliseconds cooldown) noexcept
    : cooldown_(cooldown < std::chrono::milliseconds(0)
                    ? std::chrono::milliseconds(0)
                    : cooldown) {}

bool HysteresisFilter::Update(
    bool newState, std::chrono::steady_clock::time_point now) noexcept {
    // 首次调用无历史基线，立即生效。
    if (!hasState_) {
        state_ = newState;
        lastChange_ = now;
        hasState_ = true;
        return state_;
    }
    if (newState == state_) {
        return state_;
    }
    // 距上次切换不足冷却期：抑制切换，保持旧状态。
    if ((now - lastChange_) < cooldown_) {
        return state_;
    }
    state_ = newState;
    lastChange_ = now;
    return state_;
}

bool HysteresisFilter::State() const noexcept {
    return state_;
}

void HysteresisFilter::Reset() noexcept {
    state_ = false;
    hasState_ = false;
    lastChange_ = {};
}

namespace {

// 两次决策是否视为同一决策：action + gameId + reasonCode 全同。
bool SameDecision(const PolicyDecision& a, const PolicyDecision& b) noexcept {
    return a.action == b.action && a.gameId == b.gameId &&
           a.reasonCode == b.reasonCode;
}

} // namespace

PolicyEvaluator::PolicyEvaluator(
    PolicyThresholds thresholds,
    std::chrono::milliseconds cooldown) noexcept
    : thresholds_(thresholds),
      cooldown_(cooldown < std::chrono::milliseconds(0)
                    ? std::chrono::milliseconds(0)
                    : cooldown),
      filter_(cooldown_) {}

PolicyEvaluation PolicyEvaluator::Evaluate(
    const PolicyInput& input,
    std::chrono::steady_clock::time_point now) noexcept {
    PolicyDecision fresh = EvaluatePolicy(input);
    if (fresh.action != PolicyAction::NoOp) {
        fresh.ttlMs = cooldown_; // 建议有效时长 = 防抖冷却期
    }

    // 首次求值：无历史基线，直接生效，并建立“是否行动”防抖基线。
    if (!hasLast_) {
        hasLast_ = true;
        lastDecision_ = fresh;
        // 仅建立“是否行动”防抖基线，不影响首次求值结果。
        (void)filter_.Update(fresh.action != PolicyAction::NoOp, now);
        return PolicyEvaluation{fresh, false};
    }

    // 决策未变（含 reasonCode 未变）：直接输出，不驱动防抖。
    if (SameDecision(lastDecision_, fresh)) {
        return PolicyEvaluation{fresh, false};
    }

    // 决策变化：以"是否行动"驱动防抖。被抑制则保持上一次有效输出。
    const bool acting = fresh.action != PolicyAction::NoOp;
    const bool effective = filter_.Update(acting, now);
    if (effective == acting) {
        lastDecision_ = fresh;
        return PolicyEvaluation{fresh, false};
    }
    return PolicyEvaluation{lastDecision_, true};
}

void PolicyEvaluator::Reset() noexcept {
    hasLast_ = false;
    lastDecision_ = PolicyDecision{};
    filter_.Reset();
}

bool PolicyEvaluator::IsActing() const noexcept {
    return filter_.State();
}

} // namespace optimizer::policy
