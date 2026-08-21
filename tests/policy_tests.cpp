#include "policy/policy_engine.hpp"

#include <chrono>
#include <cwchar>
#include <iostream>

namespace {

using optimizer::policy::ActionToString;
using optimizer::policy::ClassifyPressure;
using optimizer::policy::ComputeMemoryMarginPercent;
using optimizer::policy::EvaluatePolicy;
using optimizer::policy::GameFocus;
using optimizer::policy::HysteresisFilter;
using optimizer::policy::PolicyAction;
using optimizer::policy::PolicyDecision;
using optimizer::policy::PolicyEvaluator;
using optimizer::policy::PolicyInput;
using optimizer::policy::PolicyThresholds;
using optimizer::policy::PressureToString;
using optimizer::policy::ResourcePressure;

using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::steady_clock::time_point;

TimePoint T0() {
    return TimePoint{};
}

// 构造求值输入（running=false 时 gameId 留空，对应"无游戏"）。
PolicyInput MakeInput(ResourcePressure pressure, bool running, bool foreground,
                      bool pauseWhenBackground) {
    PolicyInput input;
    input.pressure = pressure;
    if (running) {
        input.game.gameId = "g";
    }
    input.game.running = running;
    input.game.foreground = foreground;
    input.game.pauseWhenBackground = pauseWhenBackground;
    return input;
}

// ---------- ComputeMemoryMarginPercent ----------

bool TestMarginPercent() {
    if (ComputeMemoryMarginPercent(0, 100).Value() != 0) {
        return false;
    }
    if (ComputeMemoryMarginPercent(100, 100).Value() != 100) {
        return false;
    }
    if (ComputeMemoryMarginPercent(50, 100).Value() != 50) {
        return false;
    }
    if (ComputeMemoryMarginPercent(1, 3).Value() != 33) { // 向下取整
        return false;
    }
    // 大数不溢出：123456789*100 = 12345678900 < 2^64。
    if (ComputeMemoryMarginPercent(123456789ULL, 987654321ULL).Value() != 12) {
        return false;
    }
    // 无效指标：total == 0 / available > total。
    if (ComputeMemoryMarginPercent(0, 0).HasValue()) {
        return false;
    }
    if (ComputeMemoryMarginPercent(101, 100).HasValue()) {
        return false;
    }
    return true;
}

// ---------- ClassifyPressure ----------

bool TestClassifyBoundaries() {
    // 默认阈值 30/15/5：严格大于语义，== 阈值落入下一级。
    const PolicyThresholds t;
    if (ClassifyPressure(100, t).Value() != ResourcePressure::Comfortable) {
        return false;
    }
    if (ClassifyPressure(31, t).Value() != ResourcePressure::Comfortable) {
        return false;
    }
    if (ClassifyPressure(30, t).Value() != ResourcePressure::Adequate) {
        return false;
    }
    if (ClassifyPressure(16, t).Value() != ResourcePressure::Adequate) {
        return false;
    }
    if (ClassifyPressure(15, t).Value() != ResourcePressure::Tight) {
        return false;
    }
    if (ClassifyPressure(6, t).Value() != ResourcePressure::Tight) {
        return false;
    }
    if (ClassifyPressure(5, t).Value() != ResourcePressure::Critical) {
        return false;
    }
    if (ClassifyPressure(0, t).Value() != ResourcePressure::Critical) {
        return false;
    }
    return true;
}

bool TestClassifyCustomThresholds() {
    // 自定义阈值 80/50/20。
    PolicyThresholds t;
    t.comfortableMarginPercent = 80;
    t.adequateMarginPercent = 50;
    t.tightMarginPercent = 20;
    if (ClassifyPressure(100, t).Value() != ResourcePressure::Comfortable) {
        return false;
    }
    if (ClassifyPressure(80, t).Value() != ResourcePressure::Adequate) {
        return false;
    }
    if (ClassifyPressure(50, t).Value() != ResourcePressure::Tight) {
        return false;
    }
    return ClassifyPressure(20, t).Value() == ResourcePressure::Critical;
}

bool TestClassifyInvalidMargin() {
    const PolicyThresholds t;
    return !ClassifyPressure(-1, t).HasValue() &&
           !ClassifyPressure(101, t).HasValue();
}

bool TestClassifyInvalidThresholds() {
    // 违序阈值：adequate >= comfortable（防御手工构造，配置层已拦截）。
    PolicyThresholds bad;
    bad.adequateMarginPercent = 40;
    return !ClassifyPressure(50, bad).HasValue();
}

// ---------- 名称映射 ----------

bool TestPressureToString() {
    return std::wcscmp(PressureToString(ResourcePressure::Comfortable),
                       L"comfortable") == 0 &&
           std::wcscmp(PressureToString(ResourcePressure::Adequate),
                       L"adequate") == 0 &&
           std::wcscmp(PressureToString(ResourcePressure::Tight), L"tight") ==
               0 &&
           std::wcscmp(PressureToString(ResourcePressure::Critical),
                       L"critical") == 0;
}

bool TestActionToString() {
    return std::wcscmp(ActionToString(PolicyAction::NoOp), L"NoOp") == 0 &&
           std::wcscmp(ActionToString(PolicyAction::Notify), L"Notify") == 0 &&
           std::wcscmp(ActionToString(PolicyAction::SuggestMemoryTune),
                       L"SuggestMemoryTune") == 0 &&
           std::wcscmp(ActionToString(PolicyAction::SuggestPriorityBoost),
                       L"SuggestPriorityBoost") == 0;
}

// ---------- EvaluatePolicy 规则 ----------

bool TestEvaluateNoGame() {
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Critical, /*running=*/false, false, true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "no_game" &&
           d.gameId.empty();
}

bool TestEvaluateCriticalNotifyRegardlessOfBackgroundPause() {
    // 危急优先提示，不受后台暂停限制。
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Critical, /*running=*/true,
        /*foreground=*/false, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::Notify && d.reasonCode == "mem_critical";
}

bool TestEvaluateBackgroundPauseNoOp() {
    // 后台 + 配置后台暂停 -> 不产生建议（即使余量紧张）。
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Tight, /*running=*/true,
        /*foreground=*/false, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "game_background";
}

bool TestEvaluateBackgroundNoPauseSuggests() {
    // 后台 + 未配置暂停 -> 仍按压力分级给建议。
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Tight, /*running=*/true,
        /*foreground=*/false, /*pauseWhenBackground=*/false));
    return d.action == PolicyAction::SuggestMemoryTune &&
           d.reasonCode == "mem_tight" && d.gameId == "g";
}

bool TestEvaluateTightForegroundSuggests() {
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Tight, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::SuggestMemoryTune &&
           d.reasonCode == "mem_tight";
}

bool TestEvaluateAdequateNoOp() {
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Adequate, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "mem_ok";
}

bool TestEvaluateComfortableNoOp() {
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Comfortable, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "mem_ok";
}

// ---------- HysteresisFilter ----------

bool TestHysteresisFirstApply() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    // 首次调用无历史基线，立即生效。
    return filter.Update(true, T0()) == true && filter.State() == true;
}

bool TestHysteresisSameStateStable() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    filter.Update(true, T0());
    // 同状态原样返回，不重置切换计时。
    return filter.Update(true, T0() + std::chrono::milliseconds(4000)) == true;
}

bool TestHysteresisSuppressThenApply() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    filter.Update(true, T0());
    // 冷却期内切换被抑制，保持旧状态。
    if (filter.Update(false, T0() + std::chrono::milliseconds(1000)) != true) {
        return false;
    }
    if (filter.State() != true) {
        return false;
    }
    // 冷却期结束（>= 5000ms）后切换生效。
    if (filter.Update(false, T0() + std::chrono::milliseconds(6000)) != false) {
        return false;
    }
    if (filter.State() != false) {
        return false;
    }
    // 刚切换后又切回：仍在冷却期内，抑制。
    if (filter.Update(true, T0() + std::chrono::milliseconds(7000)) != false) {
        return false;
    }
    // 冷却期结束，切回生效。
    return filter.Update(true, T0() + std::chrono::milliseconds(12000)) == true;
}

bool TestHysteresisZeroCooldown() {
    HysteresisFilter filter(std::chrono::milliseconds(0));
    filter.Update(true, T0());
    // 0 冷却：任何切换立即生效。
    return filter.Update(false, T0() + std::chrono::milliseconds(1)) == false;
}

bool TestHysteresisReset() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    filter.Update(true, T0());
    filter.Reset();
    if (filter.State() != false) {
        return false;
    }
    // Reset 后再次调用按首次处理，立即生效。
    return filter.Update(true, T0()) == true;
}

// ---------- PolicyEvaluator ----------

bool TestEvaluatorFirstApplies() {
    PolicyEvaluator evaluator({}, std::chrono::milliseconds(5000));
    const auto evaluation = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        T0());
    return !evaluation.suppressed &&
           evaluation.decision.action == PolicyAction::SuggestMemoryTune &&
           evaluation.decision.ttlMs == std::chrono::milliseconds(5000) &&
           evaluator.IsActing();
}

bool TestEvaluatorFlapSuppressed() {
    PolicyEvaluator evaluator({}, std::chrono::milliseconds(5000));
    const auto t0 = T0();
    // t0：余量一般 -> NoOp（首次生效）。
    auto e1 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0);
    if (e1.suppressed || e1.decision.action != PolicyAction::NoOp) {
        return false;
    }
    // t0+1s：余量紧张 -> 切换在冷却期内被抑制，输出保持上一次有效决策（NoOp）。
    auto e2 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(1));
    if (!e2.suppressed || e2.decision.action != PolicyAction::NoOp) {
        return false;
    }
    if (evaluator.IsActing()) {
        return false; // 行动态未切换
    }
    // t0+6s：冷却期结束，切换生效。
    auto e3 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(6));
    if (e3.suppressed ||
        e3.decision.action != PolicyAction::SuggestMemoryTune) {
        return false;
    }
    if (!evaluator.IsActing()) {
        return false;
    }
    // t0+7s：切回 NoOp 在冷却期内被抑制，保持行动态。
    auto e4 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(7));
    if (!e4.suppressed ||
        e4.decision.action != PolicyAction::SuggestMemoryTune) {
        return false;
    }
    // t0+12s：冷却期结束，切回 NoOp 生效。
    auto e5 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(12));
    return !e5.suppressed && e5.decision.action == PolicyAction::NoOp;
}

bool TestEvaluatorActionSwitchWhileActing() {
    // Notify -> SuggestMemoryTune 同属行动态，切换不被防抖抑制。
    PolicyEvaluator evaluator({}, std::chrono::milliseconds(5000));
    const auto t0 = T0();
    auto e1 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Critical, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0);
    if (e1.suppressed || e1.decision.action != PolicyAction::Notify) {
        return false;
    }
    auto e2 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(1));
    return !e2.suppressed &&
           e2.decision.action == PolicyAction::SuggestMemoryTune;
}

bool TestEvaluatorReset() {
    PolicyEvaluator evaluator({}, std::chrono::milliseconds(5000));
    const auto t0 = T0();
    evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0);
    // 冷却期内切换被抑制。
    auto suppressed = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(1));
    if (!suppressed.suppressed) {
        return false;
    }
    // Reset 后按首次处理，切换立即生效。
    evaluator.Reset();
    auto applied = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/true, /*pauseWhenBackground=*/true),
        t0 + std::chrono::seconds(1));
    return !applied.suppressed &&
           applied.decision.action == PolicyAction::SuggestMemoryTune &&
           evaluator.IsActing();
}

} // namespace

int wmain() {
    int failed = 0;
    const auto run = [&failed](const wchar_t* name, bool (*test)()) {
        const bool passed = test();
        std::wcout << (passed ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
        if (!passed) {
            ++failed;
        }
    };

    run(L"ComputeMemoryMarginPercent", &TestMarginPercent);
    run(L"ClassifyPressure boundaries", &TestClassifyBoundaries);
    run(L"ClassifyPressure custom thresholds", &TestClassifyCustomThresholds);
    run(L"ClassifyPressure rejects invalid margin", &TestClassifyInvalidMargin);
    run(L"ClassifyPressure rejects invalid thresholds",
        &TestClassifyInvalidThresholds);
    run(L"PressureToString names", &TestPressureToString);
    run(L"ActionToString names", &TestActionToString);
    run(L"EvaluatePolicy no game", &TestEvaluateNoGame);
    run(L"EvaluatePolicy critical notifies regardless of background pause",
        &TestEvaluateCriticalNotifyRegardlessOfBackgroundPause);
    run(L"EvaluatePolicy background pause is NoOp",
        &TestEvaluateBackgroundPauseNoOp);
    run(L"EvaluatePolicy background without pause suggests",
        &TestEvaluateBackgroundNoPauseSuggests);
    run(L"EvaluatePolicy tight foreground suggests", &TestEvaluateTightForegroundSuggests);
    run(L"EvaluatePolicy adequate is NoOp", &TestEvaluateAdequateNoOp);
    run(L"EvaluatePolicy comfortable is NoOp", &TestEvaluateComfortableNoOp);
    run(L"HysteresisFilter first call applies", &TestHysteresisFirstApply);
    run(L"HysteresisFilter same state stable", &TestHysteresisSameStateStable);
    run(L"HysteresisFilter suppress then apply", &TestHysteresisSuppressThenApply);
    run(L"HysteresisFilter zero cooldown", &TestHysteresisZeroCooldown);
    run(L"HysteresisFilter reset", &TestHysteresisReset);
    run(L"PolicyEvaluator first call applies", &TestEvaluatorFirstApplies);
    run(L"PolicyEvaluator flapping suppressed", &TestEvaluatorFlapSuppressed);
    run(L"PolicyEvaluator action switch while acting",
        &TestEvaluatorActionSwitchWhileActing);
    run(L"PolicyEvaluator reset", &TestEvaluatorReset);
    return failed == 0 ? 0 : 1;
}
