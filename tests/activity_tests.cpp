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
    return failed == 0 ? 0 : 1;
}
