#include "policy/policy_engine.hpp"
#include "policy/safety_gates.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
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

bool TestEvaluateAdequateForegroundBoosts() {
    // PWR-002：余量充足 + 前台游戏 -> 建议优先级提升（Layer 3）。
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Adequate, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::SuggestPriorityBoost &&
           d.reasonCode == "prio_boost" && d.gameId == "g";
}

bool TestEvaluateComfortableForegroundBoosts() {
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Comfortable, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::SuggestPriorityBoost &&
           d.reasonCode == "prio_boost";
}

bool TestEvaluateBackgroundNoPauseAdequateNoOp() {
    // 后台未暂停 + 余量充足：仅前台提升，后台不提升 -> NoOp。
    const auto d = EvaluatePolicy(MakeInput(
        ResourcePressure::Adequate, /*running=*/true,
        /*foreground=*/false, /*pauseWhenBackground=*/false));
    return d.action == PolicyAction::NoOp && d.reasonCode == "mem_ok";
}

// ---------- ACT-004：用户在场门禁（user_away） ----------

PolicyInput AwayInput(ResourcePressure pressure, bool running,
                      bool foreground, bool pauseWhenBackground) {
    PolicyInput input =
        MakeInput(pressure, running, foreground, pauseWhenBackground);
    input.userPresent = false; // 用户不在场（AFK/锁屏/断开/在场未知）
    return input;
}

bool TestEvaluateUserAwaySuppressesTune() {
    // 紧张 + 前台 + 不在场：原 mem_tight 建议被在场门禁抑制 -> NoOp user_away。
    const auto d = EvaluatePolicy(AwayInput(
        ResourcePressure::Tight, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "user_away" &&
           d.gameId == "g";
}

bool TestEvaluateUserAwaySuppressesBoost() {
    // 余量充足 + 前台 + 不在场：原 prio_boost 建议被抑制 -> NoOp user_away。
    const auto d = EvaluatePolicy(AwayInput(
        ResourcePressure::Comfortable, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "user_away";
}

bool TestEvaluateCriticalNotifiesEvenWhenAway() {
    // 危急提示优先于在场门禁（危急信息不因用户暂离而丢失）。
    const auto d = EvaluatePolicy(AwayInput(
        ResourcePressure::Critical, /*running=*/true,
        /*foreground=*/true, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::Notify && d.reasonCode == "mem_critical";
}

bool TestEvaluateNoGameEvenWhenAway() {
    // 无游戏运行优先于在场门禁。
    const auto d = EvaluatePolicy(AwayInput(
        ResourcePressure::Comfortable, /*running=*/false, false, true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "no_game" &&
           d.gameId.empty();
}

bool TestEvaluateUserAwayPrecedesBackgroundPause() {
    // 不在场优先于后台暂停规则：后台 + 暂停 + 不在场 -> user_away（而非 game_background）。
    const auto d = EvaluatePolicy(AwayInput(
        ResourcePressure::Comfortable, /*running=*/true,
        /*foreground=*/false, /*pauseWhenBackground=*/true));
    return d.action == PolicyAction::NoOp && d.reasonCode == "user_away";
}

// ---------- HysteresisFilter ----------

bool TestHysteresisFirstApply() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    // 首次调用无历史基线，立即生效。
    return filter.Update(true, T0()) == true && filter.State() == true;
}

bool TestHysteresisSameStateStable() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    (void)filter.Update(true, T0());
    // 同状态原样返回，不重置切换计时。
    return filter.Update(true, T0() + std::chrono::milliseconds(4000)) == true;
}

bool TestHysteresisSuppressThenApply() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    (void)filter.Update(true, T0());
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
    (void)filter.Update(true, T0());
    // 0 冷却：任何切换立即生效。
    return filter.Update(false, T0() + std::chrono::milliseconds(1)) == false;
}

bool TestHysteresisReset() {
    HysteresisFilter filter(std::chrono::milliseconds(5000));
    (void)filter.Update(true, T0());
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
    // t0：后台未暂停 + 余量一般 -> NoOp（mem_ok，非行动态，首次生效）。
    auto e1 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
        t0);
    if (e1.suppressed || e1.decision.action != PolicyAction::NoOp) {
        return false;
    }
    // t0+1s：余量紧张 -> 切换在冷却期内被抑制，输出保持上一次有效决策（NoOp）。
    auto e2 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
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
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
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
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
        t0 + std::chrono::seconds(7));
    if (!e4.suppressed ||
        e4.decision.action != PolicyAction::SuggestMemoryTune) {
        return false;
    }
    // t0+12s：冷却期结束，切回 NoOp 生效。
    auto e5 = evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
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
    // t0：后台未暂停 + 余量一般 -> NoOp（非行动态，首次生效）。
    (void)evaluator.Evaluate(
        MakeInput(ResourcePressure::Adequate, /*running=*/true,
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
        t0);
    // 冷却期内切换被抑制。
    auto suppressed = evaluator.Evaluate(
        MakeInput(ResourcePressure::Tight, /*running=*/true,
                  /*foreground=*/false, /*pauseWhenBackground=*/false),
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

// ---------- 门禁判定（六道门，只读判定） ----------

bool TestEvaluateGatesAllPass() {
    using optimizer::policy::EvaluateGates;
    using optimizer::policy::GateInputs;
    GateInputs inputs;
    inputs.compileTime = true;
    inputs.config = true;
    inputs.commandLine = true;
    inputs.permissionAndEnvironment = true;
    inputs.audit = true;
    inputs.cooldown = true;
    const auto evaluation = EvaluateGates(inputs);
    return evaluation.allowed && !evaluation.firstBlocking.has_value() &&
           evaluation.gates.audit;
}

bool TestEvaluateGatesFirstBlockingOrder() {
    using optimizer::policy::EvaluateGates;
    using optimizer::policy::GateId;
    using optimizer::policy::GateInputs;
    using optimizer::policy::GateIdToString;
    // 全 false：首个阻塞门是顺序上的第一道（编译期开关）。
    const auto none = EvaluateGates(GateInputs{});
    if (none.allowed || !none.firstBlocking.has_value() ||
        *none.firstBlocking != GateId::CompileTime ||
        std::string(GateIdToString(*none.firstBlocking)) != "compile") {
        return false;
    }
    // 只开前 2 道：首个阻塞门应是第 3 道（命令行确认）。
    GateInputs partial;
    partial.compileTime = true;
    partial.config = true;
    const auto third = EvaluateGates(partial);
    if (third.allowed || !third.firstBlocking.has_value() ||
        *third.firstBlocking != GateId::CommandLine) {
        return false;
    }
    // 只缺最后一道（冷却）：其余全通过仍不得放行。
    GateInputs withoutCooldown;
    withoutCooldown.compileTime = true;
    withoutCooldown.config = true;
    withoutCooldown.commandLine = true;
    withoutCooldown.permissionAndEnvironment = true;
    withoutCooldown.audit = true;
    const auto cooldownBlocked = EvaluateGates(withoutCooldown);
    return !cooldownBlocked.allowed && cooldownBlocked.firstBlocking.has_value() &&
           *cooldownBlocked.firstBlocking == GateId::Cooldown;
}

bool TestEvaluateEnvironmentGate() {
    using optimizer::policy::EnvironmentFacts;
    using optimizer::policy::EvaluateEnvironmentGate;
    // 全部就绪：唯一放行的组合。
    EnvironmentFacts ok;
    ok.factsKnown = true;
    ok.osSupported = true;
    ok.interactiveSession = true;
    const bool open = EvaluateEnvironmentGate(ok);
    // 未知事实 / 电池 / 远程 / 锁屏 / 非交互：逐一不得通过。
    EnvironmentFacts unknown;
    const bool unknownBlocked = !EvaluateEnvironmentGate(unknown);
    EnvironmentFacts battery = ok;
    battery.onBattery = true;
    EnvironmentFacts remote = ok;
    remote.remoteSession = true;
    EnvironmentFacts locked = ok;
    locked.sessionLocked = true;
    EnvironmentFacts backdrop = ok;
    backdrop.interactiveSession = false;
    EnvironmentFacts unsupported = ok;
    unsupported.osSupported = false;
    return open && unknownBlocked && !EvaluateEnvironmentGate(battery) &&
           !EvaluateEnvironmentGate(remote) && !EvaluateEnvironmentGate(locked) &&
           !EvaluateEnvironmentGate(backdrop) &&
           !EvaluateEnvironmentGate(unsupported);
}

bool TestCooldownGateAndLedger() {
    using optimizer::policy::CooldownLedger;
    using optimizer::policy::EvaluateCooldownGate;
    using optimizer::policy::ReadCooldownLedger;
    using optimizer::policy::WriteCooldownLedger;
    const std::chrono::seconds cooldown{15 * 60};
    const std::int64_t now = 1'700'000'000;
    // 无记录：不拦。
    CooldownLedger empty;
    const bool noRecord = EvaluateCooldownGate(empty, "memory.clean", now, cooldown);
    // 窗口内：拦；恰好到边界：放行；时钟回拨：保守拦。
    CooldownLedger recent;
    recent.lastRunUnixSeconds["memory.clean"] = now - 60;
    CooldownLedger boundary;
    boundary.lastRunUnixSeconds["memory.clean"] = now - 900;
    CooldownLedger future;
    future.lastRunUnixSeconds["memory.clean"] = now + 10;
    const bool blocked = !EvaluateCooldownGate(recent, "memory.clean", now, cooldown);
    const bool boundaryOpen = EvaluateCooldownGate(boundary, "memory.clean", now, cooldown);
    const bool rollbackBlocked = !EvaluateCooldownGate(future, "memory.clean", now, cooldown);
    // 其它能力互不影响。
    const bool otherOpen = EvaluateCooldownGate(recent, "power.scheme", now, cooldown);
    // 台账往返（每用户文件）。
    std::error_code ec;
    const auto path = std::filesystem::temp_directory_path(ec) /
                      (std::wstring(L"cpo_cooldown_") +
                       std::to_wstring(::GetCurrentProcessId()) + L".txt");
    if (ec) {
        return false;
    }
    std::filesystem::remove(path, ec);
    CooldownLedger toWrite;
    toWrite.lastRunUnixSeconds["memory.clean"] = now - 30;
    const auto written = WriteCooldownLedger(path, toWrite);
    const bool writeOk = static_cast<bool>(written);
    const auto read = ReadCooldownLedger(path);
    const bool roundTrip = writeOk && read &&
                           read.Value().lastRunUnixSeconds.size() == 1 &&
                           read.Value().lastRunUnixSeconds.at("memory.clean") == now - 30;
    // 缺失文件 = 空台账（不是错误）；空路径 = Validation。
    std::filesystem::remove(path, ec);
    const auto missing = ReadCooldownLedger(path);
    const auto emptyPath = ReadCooldownLedger({});
    // 编译期开关：默认关（与宏默认 0 一致）。
    const bool compiledOff = !optimizer::policy::MemoryCleanCompiledIn();
    return noRecord && blocked && boundaryOpen && rollbackBlocked && otherOpen &&
           roundTrip && missing && missing.Value().lastRunUnixSeconds.empty() &&
           !emptyPath && compiledOff;
}

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
    run(L"EvaluatePolicy adequate foreground boosts", &TestEvaluateAdequateForegroundBoosts);
    run(L"EvaluatePolicy comfortable foreground boosts",
        &TestEvaluateComfortableForegroundBoosts);
    run(L"EvaluatePolicy background no-pause adequate is NoOp",
        &TestEvaluateBackgroundNoPauseAdequateNoOp);
    run(L"EvaluatePolicy user away suppresses tune", &TestEvaluateUserAwaySuppressesTune);
    run(L"EvaluatePolicy user away suppresses boost",
        &TestEvaluateUserAwaySuppressesBoost);
    run(L"EvaluatePolicy critical notifies even when away",
        &TestEvaluateCriticalNotifiesEvenWhenAway);
    run(L"EvaluatePolicy no game even when away", &TestEvaluateNoGameEvenWhenAway);
    run(L"EvaluatePolicy user away precedes background pause",
        &TestEvaluateUserAwayPrecedesBackgroundPause);
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
    run(L"evaluate gates all pass", &TestEvaluateGatesAllPass);
    run(L"evaluate gates first blocking order",
        &TestEvaluateGatesFirstBlockingOrder);
    run(L"evaluate environment gate", &TestEvaluateEnvironmentGate);
    run(L"cooldown gate and ledger", &TestCooldownGateAndLedger);
    return failed == 0 ? 0 : 1;
}
