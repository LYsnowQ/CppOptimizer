#include "activity/user_activity.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::ErrorDomain;
using optimizer::common::Result;
using optimizer::activity::ActivityState;
using optimizer::activity::ActivityWindowSummary;
using optimizer::activity::ClassifyActivity;
using optimizer::activity::CreateWin32LastInputBackend;
using optimizer::activity::IdleMilliseconds;
using optimizer::activity::InputActivitySample;
using optimizer::activity::LastInputBackend;
using optimizer::activity::ObserveActivity;
using optimizer::activity::TickDeltaMs;
using optimizer::activity::ActivityStateToString;
using optimizer::activity::ClassifyContextState;
using optimizer::activity::ActivityContextSummary;
using optimizer::activity::ObserveActivityContext;
using optimizer::activity::SessionContext;
using optimizer::activity::SessionLinkState;
using optimizer::activity::SessionLinkStateToString;
using optimizer::activity::SessionProbe;
using optimizer::activity::LinkStateFromWtsValue;

// 可注入 fake：按注入序列返回样本，指定下标强制查询失败（Unknown 降级路径）。
class FakeActivityBackend final : public LastInputBackend {
public:
    std::vector<InputActivitySample> samples;
    std::vector<std::size_t> failAt; // 这些查询下标返回 Failure

    Result<InputActivitySample> Query() override {
        const std::size_t index = queryCount;
        ++queryCount;
        for (const std::size_t fail : failAt) {
            if (fail == index) {
                return Result<InputActivitySample>::Failure(Error::FromWin32(
                    5, "FakeActivityBackend")); // ERROR_ACCESS_DENIED
            }
        }
        if (index >= samples.size()) {
            return Result<InputActivitySample>::Failure(
                Error::Validation("FakeActivityBackend", L"样本耗尽"));
        }
        return Result<InputActivitySample>::Success(samples[index]);
    }

    std::size_t queryCount = 0;
};

bool TestTickDeltaBasic() {
    return TickDeltaMs(100, 150) == 50 &&
           IdleMilliseconds(150, 100) == 50 &&
           TickDeltaMs(150, 100) == 0; // toTick 早于 fromTick：回退按 0
}

bool TestTickDeltaWrapAround() {
    // 32 位回绕：lastInput 在 UINT32_MAX 附近，now 已回绕到 10：真实间隔约 16ms。
    const std::uint32_t last = std::numeric_limits<std::uint32_t>::max() - 5u;
    const std::uint32_t now = 10u;
    return TickDeltaMs(last, now) == 16 &&
           IdleMilliseconds(now, last) == 16;
}

bool TestClassifyBoundary() {
    // idleMs == 阈值 -> Idle；小于阈值 -> Active。
    return ClassifyActivity(2000, 1000, 1000) == ActivityState::Idle &&
           ClassifyActivity(1999, 1000, 1000) == ActivityState::Active &&
           ClassifyActivity(150, 100, 0) == ActivityState::Idle;
}

bool TestClassifyWrapActive() {
    const std::uint32_t last = std::numeric_limits<std::uint32_t>::max() - 100u;
    const std::uint32_t now = 20u; // 距 last 约 121ms（回绕后仍远小于阈值）
    return ClassifyActivity(now, last, 1000) == ActivityState::Active;
}

bool TestClassifyNegativeThresholdDefensive() {
    // 负阈值退化恒 Active（防御分支）。
    return ClassifyActivity(5000, 1000, -1) == ActivityState::Active;
}

bool TestStateToString() {
    return std::wstring(ActivityStateToString(ActivityState::Unknown)) ==
               L"unknown" &&
           std::wstring(ActivityStateToString(ActivityState::Active)) ==
               L"active" &&
           std::wstring(ActivityStateToString(ActivityState::Idle)) == L"idle";
}

bool TestObserveWindowCountsAndUnknown() {
    // 窗口内样本：Active / Idle（等于阈值）/ Unknown（查询失败降级，不伪装）。
    auto fake = std::make_shared<FakeActivityBackend>();
    fake->samples.push_back({5000, 4500}); // idle 500ms < 1000 -> Active
    fake->samples.push_back({6000, 5000}); // idle 1000ms == 1000 -> Idle
    fake->failAt.push_back(2);             // 第 3 次查询失败 -> Unknown
    std::vector<ActivityState> seen;
    std::vector<std::int64_t> idleSeen;
    const auto result = ObserveActivity(
        *fake, 3, std::chrono::milliseconds(0), 1000,
        [&seen, &idleSeen](ActivityState state, std::int64_t idleMs) {
            seen.push_back(state);
            idleSeen.push_back(idleMs);
        });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 3 && summary.active == 1 && summary.idle == 1 &&
           summary.unknown == 1 && seen.size() == 3 &&
           seen[0] == ActivityState::Active && seen[1] == ActivityState::Idle &&
           seen[2] == ActivityState::Unknown && idleSeen[1] == 1000 &&
           fake->queryCount == 3;
}

bool TestObserveRejectsZeroSamples() {
    // 零样本 Validation 拒绝且不查询后端。
    auto fake = std::make_shared<FakeActivityBackend>();
    const auto result = ObserveActivity(
        *fake, 0, std::chrono::milliseconds(0), 1000, nullptr);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation &&
           fake->queryCount == 0;
}

bool TestObserveRecordsMultipleStates() {
    // 混合样本：active/active/idle；汇总与回调一致。
    auto fake = std::make_shared<FakeActivityBackend>();
    fake->samples.push_back({1000, 0});
    fake->samples.push_back({2000, 0});
    fake->samples.push_back({10000, 0});
    std::size_t calls = 0;
    const auto result = ObserveActivity(
        *fake, 3, std::chrono::milliseconds(0), 5000,
        [&calls](ActivityState, std::int64_t) { ++calls; });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 3 && summary.active == 2 && summary.idle == 1 &&
           summary.unknown == 0 && calls == 3;
}

// 可注入 fake 会话上下文后端：按注入序列返回上下文，指定下标强制查询失败。
class FakeSessionProbe final : public SessionProbe {
public:
    std::vector<SessionContext> contexts;
    std::vector<std::size_t> failAt;

    Result<SessionContext> Query() override {
        const std::size_t index = queryCount;
        ++queryCount;
        for (const std::size_t fail : failAt) {
            if (fail == index) {
                return Result<SessionContext>::Failure(
                    Error::FromWin32(5, "FakeSessionProbe"));
            }
        }
        if (index >= contexts.size()) {
            return Result<SessionContext>::Failure(
                Error::Validation("FakeSessionProbe", L"上下文耗尽"));
        }
        return Result<SessionContext>::Success(contexts[index]);
    }

    std::size_t queryCount = 0;
};

SessionContext MakeContext(SessionLinkState link, bool locked = false,
                           bool remote = false) {
    SessionContext context;
    context.link = link;
    context.locked = locked;
    context.remoteSession = remote;
    return context;
}

bool TestExtendedStateToString() {
    // ACT-002：Locked/Disconnected 状态名与 SessionLinkState 状态名。
    return std::wstring(ActivityStateToString(ActivityState::Locked)) ==
               L"locked" &&
           std::wstring(ActivityStateToString(ActivityState::Disconnected)) ==
               L"disconnected" &&
           std::wstring(SessionLinkStateToString(SessionLinkState::Active)) ==
               L"active" &&
           std::wstring(SessionLinkStateToString(
               SessionLinkState::Disconnected)) == L"disconnected" &&
           std::wstring(SessionLinkStateToString(SessionLinkState::Unknown)) ==
               L"unknown" &&
           std::wstring(SessionLinkStateToString(SessionLinkState::Listen)) ==
               L"listen";
}

bool TestLinkStateFromWtsValue() {
    // WTS_CONNECTSTATE_CLASS 值映射；瞬时/未知态按 Unknown。
    return LinkStateFromWtsValue(0) == SessionLinkState::Active &&
           LinkStateFromWtsValue(1) == SessionLinkState::Connected &&
           LinkStateFromWtsValue(4) == SessionLinkState::Disconnected &&
           LinkStateFromWtsValue(5) == SessionLinkState::Idle &&
           LinkStateFromWtsValue(6) == SessionLinkState::Listen &&
           LinkStateFromWtsValue(2) == SessionLinkState::Unknown &&
           LinkStateFromWtsValue(3) == SessionLinkState::Unknown &&
           LinkStateFromWtsValue(7) == SessionLinkState::Unknown &&
           LinkStateFromWtsValue(-1) == SessionLinkState::Unknown;
}

bool TestClassifyContextStatePrecedence() {
    // 正常会话保持输入态；断开优先于锁屏；锁屏覆盖输入态；remote 不影响归类。
    const auto normal = MakeContext(SessionLinkState::Active);
    const auto disconnected = MakeContext(SessionLinkState::Disconnected);
    const auto lockedCtx = MakeContext(SessionLinkState::Active, /*locked=*/true);
    const auto both =
        MakeContext(SessionLinkState::Disconnected, /*locked=*/true);
    const auto remote = MakeContext(SessionLinkState::Active, false, true);
    return ClassifyContextState(ActivityState::Active, normal) ==
               ActivityState::Active &&
           ClassifyContextState(ActivityState::Idle, normal) ==
               ActivityState::Idle &&
           ClassifyContextState(ActivityState::Idle, disconnected) ==
               ActivityState::Disconnected &&
           ClassifyContextState(ActivityState::Unknown, disconnected) ==
               ActivityState::Disconnected &&
           ClassifyContextState(ActivityState::Idle, lockedCtx) ==
               ActivityState::Locked &&
           ClassifyContextState(ActivityState::Active, lockedCtx) ==
               ActivityState::Locked &&
           ClassifyContextState(ActivityState::Idle, both) ==
               ActivityState::Disconnected && // 断开优先
           ClassifyContextState(ActivityState::Active, remote) ==
               ActivityState::Active;
}

bool TestContextWindowCountsNormal() {
    // 会话上下文正常（Active、未锁、本地）：窗口内归类与 ObserveActivity 一致。
    auto input = std::make_shared<FakeActivityBackend>();
    input->samples.push_back({5000, 4500}); // idle 500 -> Active
    input->samples.push_back({6000, 5000}); // idle 1000 == 阈值 -> Idle
    auto session = std::make_shared<FakeSessionProbe>();
    session->contexts.push_back(MakeContext(SessionLinkState::Active));
    session->contexts.push_back(MakeContext(SessionLinkState::Active));
    std::vector<ActivityState> seen;
    const auto result = ObserveActivityContext(
        *input, *session, 2, std::chrono::milliseconds(0), 1000,
        [&seen](ActivityState state, std::int64_t) { seen.push_back(state); });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 2 && summary.active == 1 && summary.idle == 1 &&
           summary.locked == 0 && summary.disconnected == 0 &&
           summary.unknown == 0 && seen.size() == 2 &&
           seen[0] == ActivityState::Active && seen[1] == ActivityState::Idle &&
           session->queryCount == 2 && input->queryCount == 2;
}

bool TestContextWindowLockedAndDisconnected() {
    // 锁屏/断开覆盖输入态：样本 1 锁屏（即使输入近仍 Locked）、样本 2 断开。
    auto input = std::make_shared<FakeActivityBackend>();
    input->samples.push_back({5000, 4500}); // 距上次输入 500ms（若未锁会判 Active）
    input->samples.push_back({7000, 6000}); // idle 1000（断开时状态覆盖）
    auto session = std::make_shared<FakeSessionProbe>();
    session->contexts.push_back(MakeContext(SessionLinkState::Active, true));
    session->contexts.push_back(MakeContext(SessionLinkState::Disconnected));
    std::vector<ActivityState> seen;
    std::vector<std::int64_t> idleSeen;
    const auto result = ObserveActivityContext(
        *input, *session, 2, std::chrono::milliseconds(0), 1000,
        [&seen, &idleSeen](ActivityState state, std::int64_t idleMs) {
            seen.push_back(state);
            idleSeen.push_back(idleMs);
        });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 2 && summary.active == 0 && summary.idle == 0 &&
           summary.locked == 1 && summary.disconnected == 1 &&
           summary.unknown == 0 && seen.size() == 2 &&
           seen[0] == ActivityState::Locked &&
           seen[1] == ActivityState::Disconnected && idleSeen[0] == 500 &&
           idleSeen[1] == 1000; // 锁屏/断开仍透传原始 idle（展示参考）
}

bool TestContextWindowSessionFailureDegrades() {
    // 会话查询失败 -> 整样本 Unknown（不伪装成 Active/Idle），即使输入查询成功。
    auto input = std::make_shared<FakeActivityBackend>();
    input->samples.push_back({5000, 4500});
    auto session = std::make_shared<FakeSessionProbe>();
    session->failAt.push_back(0);
    std::vector<ActivityState> seen;
    const auto result = ObserveActivityContext(
        *input, *session, 1, std::chrono::milliseconds(0), 1000,
        [&seen](ActivityState state, std::int64_t) { seen.push_back(state); });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 1 && summary.unknown == 1 &&
           summary.active == 0 && seen.size() == 1 &&
           seen[0] == ActivityState::Unknown;
}

bool TestContextWindowInputFailureDegrades() {
    // 输入查询失败（会话正常）-> Unknown（与会话失败同样不伪装）。
    auto input = std::make_shared<FakeActivityBackend>();
    input->failAt.push_back(0);
    auto session = std::make_shared<FakeSessionProbe>();
    session->contexts.push_back(MakeContext(SessionLinkState::Active));
    std::vector<ActivityState> seen;
    const auto result = ObserveActivityContext(
        *input, *session, 1, std::chrono::milliseconds(0), 1000,
        [&seen](ActivityState state, std::int64_t) { seen.push_back(state); });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 1 && summary.unknown == 1 &&
           summary.active == 0 && summary.locked == 0 &&
           summary.disconnected == 0 && seen.size() == 1 &&
           seen[0] == ActivityState::Unknown;
}

bool TestContextRejectsZeroSamples() {
    // 零样本 Validation 拒绝且不查询任何后端。
    auto input = std::make_shared<FakeActivityBackend>();
    auto session = std::make_shared<FakeSessionProbe>();
    const auto result = ObserveActivityContext(
        *input, *session, 0, std::chrono::milliseconds(0), 1000, nullptr);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation &&
           input->queryCount == 0 && session->queryCount == 0;
}

bool TestContextWindowMixedCounts() {
    // 混合序列：active/locked/idle/unknown（会话第 4 样本失败）汇总与回调一致。
    auto input = std::make_shared<FakeActivityBackend>();
    input->samples.push_back({5000, 4500});  // 样本 0：Active
    input->samples.push_back({6000, 5000});  // 样本 1：idle==阈值（被锁覆盖）
    input->samples.push_back({7000, 6000});  // 样本 2：idle（正常）
    input->samples.push_back({8000, 6000});  // 样本 3：idle（会话失败 -> Unknown）
    auto session = std::make_shared<FakeSessionProbe>();
    session->contexts.push_back(MakeContext(SessionLinkState::Active));
    session->contexts.push_back(MakeContext(SessionLinkState::Active, true));
    session->contexts.push_back(MakeContext(SessionLinkState::Active));
    session->failAt.push_back(3);
    std::size_t calls = 0;
    const auto result = ObserveActivityContext(
        *input, *session, 4, std::chrono::milliseconds(0), 1000,
        [&calls](ActivityState, std::int64_t) { ++calls; });
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    return summary.samples == 4 && summary.active == 1 && summary.idle == 1 &&
           summary.locked == 1 && summary.disconnected == 0 &&
           summary.unknown == 1 && calls == 4;
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

    run(L"tick delta basic", &TestTickDeltaBasic);
    run(L"tick delta wrap around", &TestTickDeltaWrapAround);
    run(L"classify boundary", &TestClassifyBoundary);
    run(L"classify wrap active", &TestClassifyWrapActive);
    run(L"classify negative threshold defensive",
        &TestClassifyNegativeThresholdDefensive);
    run(L"state to string", &TestStateToString);
    run(L"observe window counts and unknown", &TestObserveWindowCountsAndUnknown);
    run(L"observe rejects zero samples", &TestObserveRejectsZeroSamples);
    run(L"observe records multiple states", &TestObserveRecordsMultipleStates);
    run(L"extended state to string", &TestExtendedStateToString);
    run(L"link state from wts value", &TestLinkStateFromWtsValue);
    run(L"classify context precedence", &TestClassifyContextStatePrecedence);
    run(L"context window counts normal", &TestContextWindowCountsNormal);
    run(L"context window locked and disconnected",
        &TestContextWindowLockedAndDisconnected);
    run(L"context window session failure degrades",
        &TestContextWindowSessionFailureDegrades);
    run(L"context window input failure degrades",
        &TestContextWindowInputFailureDegrades);
    run(L"context rejects zero samples", &TestContextRejectsZeroSamples);
    run(L"context window mixed counts", &TestContextWindowMixedCounts);
    return failed == 0 ? 0 : 1;
}
