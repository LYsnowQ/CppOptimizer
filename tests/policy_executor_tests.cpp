#include "policy/policy_executor.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::Result;
using optimizer::config::PriorityLevel;
using optimizer::policy::ExecutorConfig;
using optimizer::policy::ExecutorEffect;
using optimizer::policy::ExecutorTarget;
using optimizer::policy::PolicyAction;
using optimizer::policy::PolicyDecision;
using optimizer::policy::PolicyExecutor;
using optimizer::power::PowerLockType;
using optimizer::power::PowerLocker;
using optimizer::power::PowerRequestBackend;
using optimizer::priority::PriorityBackend;
using optimizer::priority::PriorityBooster;

// ---------- 电源 fake（记录调用序列） ----------

class PowerFake final : public PowerRequestBackend {
public:
    bool failNextCreate = false;
    bool failNextSet = false;
    bool failNextClear = false;

    std::uint64_t nextHandle = 1000;
    std::vector<std::string> ops; // create / set / clear / close
    std::set<std::uint64_t> open;

    Result<std::uint64_t> CreateRequest(std::wstring_view) override {
        ops.push_back("create");
        if (failNextCreate) {
            failNextCreate = false;
            return Result<std::uint64_t>::Failure(
                Error::Validation("PowerFake::CreateRequest",
                                  L"injected create failure"));
        }
        const auto h = nextHandle++;
        open.insert(h);
        return Result<std::uint64_t>::Success(h);
    }

    Result<void> SetRequest(std::uint64_t, PowerLockType) override {
        ops.push_back("set");
        if (failNextSet) {
            failNextSet = false;
            return Result<void>::Failure(
                Error::Validation("PowerFake::SetRequest",
                                  L"injected set failure"));
        }
        return Result<void>::Success();
    }

    Result<void> ClearRequest(std::uint64_t, PowerLockType) override {
        ops.push_back("clear");
        if (failNextClear) {
            failNextClear = false;
            return Result<void>::Failure(
                Error::Validation("PowerFake::ClearRequest",
                                  L"injected clear failure"));
        }
        return Result<void>::Success();
    }

    void CloseRequest(std::uint64_t handle) noexcept override {
        ops.push_back("close");
        open.erase(handle);
    }
};

// ---------- 优先级 fake（记录调用序列） ----------

class PriorityFake final : public PriorityBackend {
public:
    static constexpr std::uint32_t kNormalClass = 0x00000020u;
    static constexpr std::uint32_t kAboveNormalClass = 0x00008000u;

    bool failNextOpen = false;
    bool failNextQueryClass = false;
    bool failNextQueryCreation = false;
    bool failNextSet = false;

    std::uint64_t nextHandle = 1000;
    std::vector<std::string> ops; // open / qct / qcls / set / close
    std::set<std::uint64_t> open;
    std::map<std::uint32_t, std::uint64_t> handleByPid;
    std::map<std::uint64_t, std::uint32_t> priorityByHandle;
    std::map<std::uint64_t, std::uint64_t> creationByHandle;
    std::map<std::uint32_t, std::uint64_t> creationByPid;

    Result<std::uint64_t> OpenProcess(std::uint32_t pid) override {
        ops.push_back("open");
        if (failNextOpen) {
            failNextOpen = false;
            return Result<std::uint64_t>::Failure(
                Error::Validation("PriorityFake::OpenProcess",
                                  L"injected open failure"));
        }
        const auto h = nextHandle++;
        open.insert(h);
        handleByPid[pid] = h;
        if (const auto it = creationByPid.find(pid);
            it != creationByPid.end()) {
            creationByHandle[h] = it->second;
        }
        priorityByHandle[h] = kNormalClass;
        return Result<std::uint64_t>::Success(h);
    }

    std::uint64_t QueryCreationTime(std::uint64_t handle) override {
        ops.push_back("qct");
        if (failNextQueryCreation) {
            failNextQueryCreation = false;
            return 0;
        }
        const auto it = creationByHandle.find(handle);
        return it == creationByHandle.end() ? 0 : it->second;
    }

    std::uint32_t QueryPriorityClass(std::uint64_t handle) override {
        ops.push_back("qcls");
        if (failNextQueryClass) {
            failNextQueryClass = false;
            return 0;
        }
        const auto it = priorityByHandle.find(handle);
        return it == priorityByHandle.end() ? 0 : it->second;
    }

    Result<void> SetPriorityClass(std::uint64_t handle,
                                  std::uint32_t priorityClass) override {
        ops.push_back("set");
        if (failNextSet) {
            failNextSet = false;
            return Result<void>::Failure(
                Error::Validation("PriorityFake::SetPriorityClass",
                                  L"injected set failure"));
        }
        priorityByHandle[handle] = priorityClass;
        return Result<void>::Success();
    }

    void CloseProcess(std::uint64_t handle) noexcept override {
        ops.push_back("close");
        open.erase(handle);
    }
};

// ---------- 构造辅助 ----------

PolicyDecision MakeDecision(PolicyAction action, const char* reason = "") {
    PolicyDecision d;
    d.action = action;
    d.gameId = "g";
    d.reasonCode = reason;
    return d;
}

ExecutorTarget MakeTarget(bool running, std::uint32_t pid = 100,
                          std::uint64_t creationTime = 111) {
    ExecutorTarget t;
    t.gameId = "g";
    t.pid = pid;
    t.creationTime100ns = creationTime;
    t.running = running;
    return t;
}

struct Harness {
    std::shared_ptr<PowerFake> power = std::make_shared<PowerFake>();
    std::shared_ptr<PriorityFake> priority = std::make_shared<PriorityFake>();
    std::shared_ptr<PowerLocker> powerLocker =
        std::make_shared<PowerLocker>(power);
    std::shared_ptr<PriorityBooster> booster =
        std::make_shared<PriorityBooster>(priority);
    ExecutorConfig config;

    Harness() {
        config.priorityEnabled = true;
        config.powerExecutionRequired = true;
    }

    PolicyExecutor Make() { return PolicyExecutor(powerLocker, booster, config); }
};

// ---------- 门禁 ----------

bool TestAllGatesOffIsAdvisory() {
    // 全部门禁关闭：即使决策建议提升且游戏运行，也不触碰任何后端。
    Harness h;
    h.config.priorityEnabled = false;
    h.config.powerExecutionRequired = false;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true));
    if (!result) {
        return false;
    }
    if (h.power->ops.empty() == false || h.priority->ops.empty() == false) {
        return false;
    }
    return !executor.IsPriorityHeld() && !executor.IsPowerHeld();
}

bool TestPriorityGateOffSkipsBoost() {
    Harness h;
    h.config.priorityEnabled = false;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true));
    if (!result || !h.priority->ops.empty()) {
        return false;
    }
    return !executor.IsPriorityHeld();
}

bool TestPowerGateOffSkipsHold() {
    Harness h;
    h.config.powerExecutionRequired = false;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "mem_ok"), MakeTarget(true));
    if (!result || !h.power->ops.empty()) {
        return false;
    }
    return !executor.IsPowerHeld();
}

// ---------- 优先级执行与释放 ----------

bool TestPriorityBoostExecutes() {
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true, 100, 111));
    if (!result || !result.Value().priorityBoosted) {
        return false;
    }
    // open -> qct -> qcls -> set（提升落地）。
    if (h.priority->ops.size() != 4 ||
        h.priority->ops[0] != "open" || h.priority->ops[3] != "set") {
        return false;
    }
    if (h.priority->priorityByHandle.count(
            h.priority->handleByPid[100]) == 0) {
        return false;
    }
    return executor.IsPriorityHeld();
}

bool TestPriorityIdempotentSameTarget() {
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    const auto boost =
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost");
    if (!executor.ApplyDecision(boost, MakeTarget(true, 100, 111))) {
        return false;
    }
    const auto again = executor.ApplyDecision(boost, MakeTarget(true, 100, 111));
    if (!again) {
        return false;
    }
    if (again.Value().priorityBoosted || again.Value().priorityReleased) {
        return false; // 幂等：同目标不再重复获取/释放
    }
    // 只打开过一次。
    std::size_t opens = 0;
    for (const auto& op : h.priority->ops) {
        if (op == "open") {
            ++opens;
        }
    }
    return opens == 1;
}

bool TestPriorityReleaseOnGameStop() {
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
            MakeTarget(true, 100, 111))) {
        return false;
    }
    // 游戏退出：目标 running=false -> 自动释放（游戏退出自动释放）。
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "no_game"), MakeTarget(false));
    if (!result || !result.Value().priorityReleased) {
        return false;
    }
    if (executor.IsPriorityHeld()) {
        return false;
    }
    // 释放序列以 close 收尾，句柄清空。
    if (h.priority->ops.empty() ||
        h.priority->ops.back() != "close") {
        return false;
    }
    return h.priority->open.empty();
}

bool TestPriorityReleaseOnDecisionChange() {
    // 游戏仍运行但决策不再建议提升（如余量转紧张）-> 释放。
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
            MakeTarget(true, 100, 111))) {
        return false;
    }
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestMemoryTune, "mem_tight"),
        MakeTarget(true, 100, 111));
    if (!result || !result.Value().priorityReleased) {
        return false;
    }
    return !executor.IsPriorityHeld() && h.priority->open.empty();
}

bool TestPriorityRestartReplacesLease() {
    // 同 gameId 进程重启（pid 变化）：旧租约释放 + 新进程提升。
    Harness h;
    h.priority->creationByPid[100] = 111;
    h.priority->creationByPid[200] = 222;
    PolicyExecutor executor = h.Make();
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
            MakeTarget(true, 100, 111))) {
        return false;
    }
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true, 200, 222));
    if (!result) {
        return false;
    }
    if (!result.Value().priorityReleased || !result.Value().priorityBoosted) {
        return false;
    }
    // 旧句柄已关闭，仅新进程持有。
    if (h.priority->open.size() != 1) {
        return false;
    }
    if (h.priority->handleByPid.count(200) == 0) {
        return false;
    }
    return executor.IsPriorityHeld();
}

bool TestPriorityBoostSkippedWithoutIdentity() {
    Harness h;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true, /*pid=*/0, /*creationTime=*/0));
    if (!result || result.Value().priorityBoosted) {
        return false;
    }
    if (h.priority->ops.empty() == false) {
        return false; // 无效身份绝不触碰进程
    }
    return !result.Value().skipped.empty() && !executor.IsPriorityHeld();
}

// ---------- 电源请求 ----------

bool TestPowerHeldWhileGameRunning() {
    Harness h;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "mem_ok"), MakeTarget(true));
    if (!result || !result.Value().powerHeld) {
        return false;
    }
    // create + set（execution 请求生效）。
    if (h.power->ops.size() != 2 ||
        h.power->ops[0] != "create" || h.power->ops[1] != "set") {
        return false;
    }
    return executor.IsPowerHeld() && h.power->open.size() == 1;
}

bool TestPowerReleasedOnGameStop() {
    Harness h;
    PolicyExecutor executor = h.Make();
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::NoOp, "mem_ok"), MakeTarget(true))) {
        return false;
    }
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "no_game"), MakeTarget(false));
    if (!result || !result.Value().powerReleased) {
        return false;
    }
    if (h.power->ops.size() != 4 ||
        h.power->ops[2] != "clear" || h.power->ops[3] != "close") {
        return false;
    }
    return !executor.IsPowerHeld() && h.power->open.empty();
}

bool TestPowerIdempotentWhileRunning() {
    Harness h;
    PolicyExecutor executor = h.Make();
    const auto input = MakeTarget(true);
    if (!executor.ApplyDecision(MakeDecision(PolicyAction::NoOp, "mem_ok"),
                                input)) {
        return false;
    }
    const auto again =
        executor.ApplyDecision(MakeDecision(PolicyAction::NoOp, "mem_ok"),
                               input);
    if (!again || again.Value().powerHeld) {
        return false; // 已持有则不再重复获取
    }
    std::size_t creates = 0;
    for (const auto& op : h.power->ops) {
        if (op == "create") {
            ++creates;
        }
    }
    return creates == 1;
}

// ---------- 不触发系统修改的决策 ----------

bool TestNotifyAndMemoryTuneNeverExecute() {
    // 危急仅提示、内存维护无执行器：不触发优先级提升；
    // 电源请求按游戏生命周期持有（与决策类型无关），此处只断言 priority 不动作。
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::Notify, "mem_critical"),
            MakeTarget(true, 100, 111))) {
        return false;
    }
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::SuggestMemoryTune, "mem_tight"),
            MakeTarget(true, 100, 111))) {
        return false;
    }
    if (h.priority->ops.empty() == false) {
        return false; // 无任何优先级提升/释放动作
    }
    return !executor.IsPriorityHeld();
}

// ---------- 失败路径（失败不伪装成功） ----------

bool TestAcquireFailureNotDisguised() {
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    h.priority->failNextSet = true;
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true, 100, 111));
    if (result) {
        return false; // 应失败
    }
    if (executor.IsPriorityHeld()) {
        return false;
    }
    // 不留悬空句柄（set 失败后 close）。
    return h.priority->open.empty();
}

bool TestReleaseFailureReportedAndRetryable() {
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make();
    if (!executor.ApplyDecision(
            MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
            MakeTarget(true, 100, 111))) {
        return false;
    }
    h.priority->failNextSet = true; // 恢复（set 回原值）失败
    if (executor.ApplyDecision(
            MakeDecision(PolicyAction::NoOp, "no_game"), MakeTarget(false))
            .HasValue()) {
        return false; // 应上报失败
    }
    // 提升仍生效（恢复失败保持持有），可重试。
    if (!executor.IsPriorityHeld()) {
        return false;
    }
    const auto retry = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "no_game"), MakeTarget(false));
    if (!retry || !retry.Value().priorityReleased) {
        return false;
    }
    return !executor.IsPriorityHeld() && h.priority->open.empty();
}

bool TestPowerAcquireFailureNotDisguised() {
    Harness h;
    PolicyExecutor executor = h.Make();
    h.power->failNextCreate = true;
    if (executor.ApplyDecision(MakeDecision(PolicyAction::NoOp, "mem_ok"),
                               MakeTarget(true))
            .HasValue()) {
        return false;
    }
    return !executor.IsPowerHeld() && h.power->open.empty();
}

// ---------- IPC-017：R1 动作连续失败停摆（同一动作连续失败超阈值，docs/23 §6） ----------

bool TestConsecutiveFailuresHaltDisabledByDefault() {
    // 阈值默认 0 = 关闭：连续失败不触发停摆（零回归），每次仍如实报错并重试。
    Harness h;
    h.priority->creationByPid[100] = 111;
    PolicyExecutor executor = h.Make(); // threshold 0
    for (int i = 0; i < 6; ++i) {
        h.priority->failNextOpen = true;
        if (executor.ApplyDecision(
                MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
                MakeTarget(true, 100, 111))
                .HasValue()) {
            return false;
        }
    }
    return !executor.IsHalted();
}

bool TestConsecutiveFailuresHaltAtThreshold() {
    // 阈值 3：连续 3 次 R1 动作失败后停摆；停摆后不再调用后端，返回 success+skipped。
    Harness h;
    h.priority->creationByPid[100] = 111;
    h.config.consecutiveActionFailuresToHalt = 3;
    PolicyExecutor executor = h.Make();
    for (int i = 0; i < 3; ++i) {
        h.priority->failNextOpen = true;
        if (executor.ApplyDecision(
                MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
                MakeTarget(true, 100, 111))
                .HasValue()) {
            return false;
        }
    }
    if (!executor.IsHalted()) {
        return false;
    }
    const std::size_t opensBefore = [&h]() {
        std::size_t opens = 0;
        for (const auto& op : h.priority->ops) {
            if (op == "open") {
                ++opens;
            }
        }
        return opens;
    }();
    // 停摆后第 4 次调用（后端已不注入失败）：不再触碰后端，返回 success + skipped。
    const auto after = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true, 100, 111));
    if (!after || after.Value().skipped.find(L"halted") ==
                      std::wstring::npos) {
        return false;
    }
    std::size_t opensAfter = 0;
    for (const auto& op : h.priority->ops) {
        if (op == "open") {
            ++opensAfter;
        }
    }
    return opensAfter == opensBefore && !executor.IsPriorityHeld();
}

bool TestConsecutiveFailuresSuccessResetsCounter() {
    // 成功（含无需动作）断开失败序列：2 次失败 -> 1 次成功 -> 再 2 次失败仍未达阈值 3。
    Harness h;
    h.priority->creationByPid[100] = 111;
    h.config.consecutiveActionFailuresToHalt = 3;
    PolicyExecutor executor = h.Make();
    for (int i = 0; i < 2; ++i) {
        h.priority->failNextOpen = true;
        if (executor.ApplyDecision(
                MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
                MakeTarget(true, 100, 111))
                .HasValue()) {
            return false;
        }
    }
    // 成功（无需动作也算成功）：复位连续计数。
    const auto ok = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "mem_ok"), MakeTarget(false));
    if (!ok || executor.IsHalted()) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        h.priority->failNextOpen = true;
        if (executor.ApplyDecision(
                MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
                MakeTarget(true, 100, 111))
                .HasValue()) {
            return false;
        }
    }
    // 复位后仅累计 2 次：仍 Normal（未达阈值 3）。
    return !executor.IsHalted();
}

bool TestConsecutiveFailuresThresholdOne() {
    // 阈值 1：单次 R1 动作失败即停摆。
    Harness h;
    h.priority->creationByPid[100] = 111;
    h.config.consecutiveActionFailuresToHalt = 1;
    PolicyExecutor executor = h.Make();
    h.priority->failNextOpen = true;
    if (executor.ApplyDecision(
            MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
            MakeTarget(true, 100, 111))
            .HasValue()) {
        return false;
    }
    return executor.IsHalted();
}

bool TestResetHaltClearsHaltedExecutor() {
    // ResetHalt 清除停摆与计数：下一次 ApplyDecision 恢复正常执行。
    Harness h;
    h.priority->creationByPid[100] = 111;
    h.config.consecutiveActionFailuresToHalt = 2;
    PolicyExecutor executor = h.Make();
    for (int i = 0; i < 2; ++i) {
        h.priority->failNextOpen = true;
        if (executor.ApplyDecision(
                MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
                MakeTarget(true, 100, 111))
                .HasValue()) {
            return false;
        }
    }
    if (!executor.IsHalted()) {
        return false;
    }
    executor.ResetHalt();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
        MakeTarget(true, 100, 111));
    return !executor.IsHalted() && result &&
           result.Value().priorityBoosted && executor.IsPriorityHeld();
}

// ---------- RAII ----------

bool TestReleaseAllReleasesBoth() {
    Harness h;
    h.priority->creationByPid[100] = 111;
    {
        PolicyExecutor executor = h.Make();
        if (!executor.ApplyDecision(
                MakeDecision(PolicyAction::SuggestPriorityBoost, "prio_boost"),
                MakeTarget(true, 100, 111))) {
            return false;
        }
    } // 析构自动 ReleaseAll
    if (!h.priority->open.empty() || !h.power->open.empty()) {
        return false;
    }
    return true;
}

bool TestNoGameNoAction() {
    Harness h;
    PolicyExecutor executor = h.Make();
    const auto result = executor.ApplyDecision(
        MakeDecision(PolicyAction::NoOp, "no_game"), MakeTarget(false));
    if (!result) {
        return false;
    }
    if (!h.power->ops.empty() || !h.priority->ops.empty()) {
        return false;
    }
    return !executor.IsPriorityHeld() && !executor.IsPowerHeld();
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

    run(L"all gates off stays advisory", &TestAllGatesOffIsAdvisory);
    run(L"priority gate off skips boost", &TestPriorityGateOffSkipsBoost);
    run(L"power gate off skips hold", &TestPowerGateOffSkipsHold);
    run(L"priority boost executes", &TestPriorityBoostExecutes);
    run(L"priority idempotent same target", &TestPriorityIdempotentSameTarget);
    run(L"priority released on game stop", &TestPriorityReleaseOnGameStop);
    run(L"priority released on decision change",
        &TestPriorityReleaseOnDecisionChange);
    run(L"priority restart replaces lease", &TestPriorityRestartReplacesLease);
    run(L"priority boost skipped without identity",
        &TestPriorityBoostSkippedWithoutIdentity);
    run(L"power held while game running", &TestPowerHeldWhileGameRunning);
    run(L"power released on game stop", &TestPowerReleasedOnGameStop);
    run(L"power idempotent while running", &TestPowerIdempotentWhileRunning);
    run(L"notify and memory tune never execute",
        &TestNotifyAndMemoryTuneNeverExecute);
    run(L"acquire failure not disguised", &TestAcquireFailureNotDisguised);
    run(L"release failure reported and retryable",
        &TestReleaseFailureReportedAndRetryable);
    run(L"power acquire failure not disguised",
        &TestPowerAcquireFailureNotDisguised);
    run(L"consecutive failures halt disabled by default",
        &TestConsecutiveFailuresHaltDisabledByDefault);
    run(L"consecutive failures halt at threshold",
        &TestConsecutiveFailuresHaltAtThreshold);
    run(L"consecutive failures success resets counter",
        &TestConsecutiveFailuresSuccessResetsCounter);
    run(L"consecutive failures threshold one", &TestConsecutiveFailuresThresholdOne);
    run(L"reset halt clears halted executor", &TestResetHaltClearsHaltedExecutor);
    run(L"release all releases both", &TestReleaseAllReleasesBoth);
    run(L"no game no action", &TestNoGameNoAction);
    return failed == 0 ? 0 : 1;
}
