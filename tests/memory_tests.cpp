#include "memory/memory_tuner.hpp"
#include "memory/memory_cleaner.hpp"

#include <chrono>
#include <iostream>
#include <vector>

namespace {

bool TestBuildMemoryStatusDerivesUsedBytes() {
    const auto sampledAt = std::chrono::steady_clock::time_point{} +
                           std::chrono::seconds(7);
    auto result = optimizer::memory::BuildMemoryStatus(
        16'000,
        6'000,
        62,
        sampledAt);

    return result.HasValue() &&
           result.Value().totalPhysicalBytes == 16'000 &&
           result.Value().availablePhysicalBytes == 6'000 &&
           result.Value().usedPhysicalBytes == 10'000 &&
           result.Value().memoryLoadPercent == 62 &&
           result.Value().sampledAt == sampledAt;
}

bool TestBuildMemoryStatusRejectsZeroTotal() {
    auto result = optimizer::memory::BuildMemoryStatus(
        0,
        0,
        0,
        std::chrono::steady_clock::time_point{});

    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestBuildMemoryStatusRejectsAvailableAboveTotal() {
    auto result = optimizer::memory::BuildMemoryStatus(
        8'000,
        8'001,
        0,
        std::chrono::steady_clock::time_point{});

    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestBuildMemoryStatusRejectsPercentAboveOneHundred() {
    auto result = optimizer::memory::BuildMemoryStatus(
        8'000,
        4'000,
        101,
        std::chrono::steady_clock::time_point{});

    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestFormatBytesStaysInBytesBelowOneKiB() {
    return optimizer::memory::FormatBytes(0) == L"0 B" &&
           optimizer::memory::FormatBytes(1) == L"1 B" &&
           optimizer::memory::FormatBytes(1023) == L"1023 B";
}

bool TestFormatBytesSelectsBinaryUnits() {
    return optimizer::memory::FormatBytes(1024) == L"1.0 KiB" &&
           optimizer::memory::FormatBytes(1536) == L"1.5 KiB" &&
           optimizer::memory::FormatBytes(1'048'576) == L"1.0 MiB" &&
           optimizer::memory::FormatBytes(1'572'864) == L"1.5 MiB" &&
           optimizer::memory::FormatBytes(1'073'741'824) == L"1.0 GiB";
}

bool TestFormatBytesRoundsHalfUpToTenths() {
    // 1,101,005 字节 == 1.050003... MiB，必须向上取整为 1.1 MiB。
    return optimizer::memory::FormatBytes(1'101'005) == L"1.1 MiB";
}

bool TestIsSnapshotFreshWithinWindow() {
    const auto sampledAt = std::chrono::steady_clock::time_point{} +
                           std::chrono::seconds(100);
    return optimizer::memory::IsSnapshotFresh(
        sampledAt, sampledAt + std::chrono::seconds(5), std::chrono::seconds(10));
}

bool TestIsSnapshotFreshBoundaryIsInclusive() {
    const auto sampledAt = std::chrono::steady_clock::time_point{} +
                           std::chrono::seconds(100);
    return optimizer::memory::IsSnapshotFresh(
        sampledAt, sampledAt + std::chrono::seconds(10), std::chrono::seconds(10));
}

bool TestIsSnapshotFreshOlderThanWindow() {
    const auto sampledAt = std::chrono::steady_clock::time_point{} +
                           std::chrono::seconds(100);
    return !optimizer::memory::IsSnapshotFresh(
        sampledAt, sampledAt + std::chrono::seconds(11), std::chrono::seconds(10));
}

bool TestIsSnapshotFreshFutureSampleIsNotStale() {
    const auto sampledAt = std::chrono::steady_clock::time_point{} +
                           std::chrono::seconds(100);
    return optimizer::memory::IsSnapshotFresh(
        sampledAt, sampledAt - std::chrono::seconds(5), std::chrono::seconds(10));
}

bool TestIsSnapshotFreshZeroMaxAgeBoundary() {
    const auto sampledAt = std::chrono::steady_clock::time_point{} +
                           std::chrono::seconds(100);
    // maxAge == 0 保持边界包含：仅当下时刻新鲜，下一秒即陈旧，未来时间戳永不陈旧。
    return optimizer::memory::IsSnapshotFresh(
               sampledAt, sampledAt, std::chrono::steady_clock::duration::zero()) &&
           !optimizer::memory::IsSnapshotFresh(
               sampledAt, sampledAt + std::chrono::seconds(1),
               std::chrono::steady_clock::duration::zero()) &&
           optimizer::memory::IsSnapshotFresh(
               sampledAt, sampledAt - std::chrono::seconds(1),
               std::chrono::steady_clock::duration::zero());
}

bool TestQueryMemoryStatusReturnsValidSnapshot() {
    auto result = optimizer::memory::QueryMemoryStatus();
    if (!result.HasValue()) {
        return false;
    }

    const auto& status = result.Value();
    return status.totalPhysicalBytes > 0 &&
           status.availablePhysicalBytes <= status.totalPhysicalBytes &&
           status.usedPhysicalBytes ==
               status.totalPhysicalBytes - status.availablePhysicalBytes &&
           status.memoryLoadPercent <= 100;
}

} // namespace

// ---------- 内存清理计划（本切片：只计划，不执行） ----------

bool TestPlanMemoryCleanNoneLevel() {
    using optimizer::config::CleanLevel;
    using optimizer::memory::PlanMemoryClean;
    const auto plan = PlanMemoryClean(CleanLevel::None, CleanLevel::Light, true);
    return !plan.allowed && plan.levelWithinLimit && plan.steps.empty() &&
           plan.gatesAllowed;
}

bool TestPlanMemoryCleanGatesBlocked() {
    using optimizer::config::CleanLevel;
    using optimizer::memory::CleanKind;
    using optimizer::memory::PlanMemoryClean;
    const auto plan = PlanMemoryClean(CleanLevel::Light, CleanLevel::Light, false);
    return !plan.allowed && plan.levelWithinLimit && !plan.gatesAllowed &&
           plan.steps.size() == 1 && plan.steps[0] == CleanKind::WorkingSetTrim;
}

bool TestPlanMemoryCleanAboveLimit() {
    using optimizer::config::CleanLevel;
    using optimizer::memory::PlanMemoryClean;
    const auto plan = PlanMemoryClean(CleanLevel::Light, CleanLevel::None, true);
    return !plan.allowed && !plan.levelWithinLimit && plan.steps.empty();
}

bool TestRefusingCleanBackendIsHonest() {
    using optimizer::memory::CleanKind;
    using optimizer::memory::RefusingCleanBackend;
    const auto result = RefusingCleanBackend().Execute(CleanKind::WorkingSetTrim);
    return !result &&
           result.ErrorValue().domain ==
               optimizer::common::ErrorDomain::Unsupported;
}

bool TestCleanPlanOrchestration() {
    using optimizer::config::CleanLevel;
    using optimizer::memory::CleanBackend;
    using optimizer::memory::CleanKind;
    using optimizer::memory::ExecuteMemoryCleanPlan;
    using optimizer::memory::PlanMemoryClean;
    class FakeCleanBackend final : public CleanBackend {
    public:
        std::vector<CleanKind> calls;
        bool failFirst = false;
        [[nodiscard]] optimizer::common::Result<void> Execute(
            CleanKind kind) override {
            calls.push_back(kind);
            if (failFirst) {
                return optimizer::common::Result<void>::Failure(
                    optimizer::common::Error::FromWin32(5u, "FakeCleanBackend"));
            }
            return optimizer::common::Result<void>::Success();
        }
    };
    // ① 计划未获许可：不调用后端（“没做”与“做了但失败”必须可区分）。
    FakeCleanBackend untouched;
    const auto blocked = ExecuteMemoryCleanPlan(
        PlanMemoryClean(CleanLevel::Light, CleanLevel::Light, false), untouched);
    const bool noCalls = !blocked && untouched.calls.empty();
    // ② 许可 + 后端成功：全部步骤执行且成功。
    FakeCleanBackend okBackend;
    const auto executed = ExecuteMemoryCleanPlan(
        PlanMemoryClean(CleanLevel::Light, CleanLevel::Light, true), okBackend);
    const bool okReport = executed && executed.Value().ok &&
                          executed.Value().executed == 1 &&
                          executed.Value().succeeded == 1 &&
                          !executed.Value().hasFailedStep &&
                          okBackend.calls.size() == 1 &&
                          okBackend.calls[0] == CleanKind::WorkingSetTrim;
    // ③ 许可 + 第一步失败：失败即停，报告如实携带失败步骤与部分结果。
    FakeCleanBackend failing;
    failing.failFirst = true;
    const auto partial = ExecuteMemoryCleanPlan(
        PlanMemoryClean(CleanLevel::Light, CleanLevel::Light, true), failing);
    const bool partialReport =
        partial && !partial.Value().ok && partial.Value().hasFailedStep &&
        partial.Value().failedStep == CleanKind::WorkingSetTrim &&
        partial.Value().executed == 1 && partial.Value().succeeded == 0 &&
        failing.calls.size() == 1;
    // ④ 许可但零步骤（防御）：Validation 拒绍且不调用后端。
    optimizer::memory::MemoryCleanPlan emptyPlan;
    emptyPlan.allowed = true;
    FakeCleanBackend noSteps;
    const auto empty = ExecuteMemoryCleanPlan(emptyPlan, noSteps);
    const bool emptyRejected =
        !empty &&
        empty.ErrorValue().domain == optimizer::common::ErrorDomain::Validation &&
        noSteps.calls.empty();
    return noCalls && okReport && partialReport && emptyRejected;
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

    run(L"MemoryStatus derives used bytes", &TestBuildMemoryStatusDerivesUsedBytes);
    run(L"MemoryStatus rejects zero total", &TestBuildMemoryStatusRejectsZeroTotal);
    run(L"MemoryStatus rejects available above total", &TestBuildMemoryStatusRejectsAvailableAboveTotal);
    run(L"MemoryStatus rejects percent above 100", &TestBuildMemoryStatusRejectsPercentAboveOneHundred);
    run(L"FormatBytes stays in bytes below 1 KiB", &TestFormatBytesStaysInBytesBelowOneKiB);
    run(L"FormatBytes selects binary units", &TestFormatBytesSelectsBinaryUnits);
    run(L"FormatBytes rounds half up to tenths", &TestFormatBytesRoundsHalfUpToTenths);
    run(L"Snapshot fresh within window", &TestIsSnapshotFreshWithinWindow);
    run(L"Snapshot fresh boundary inclusive", &TestIsSnapshotFreshBoundaryIsInclusive);
    run(L"Snapshot stale after window", &TestIsSnapshotFreshOlderThanWindow);
    run(L"Snapshot future sample is not stale", &TestIsSnapshotFreshFutureSampleIsNotStale);
    run(L"Snapshot zero maxAge keeps inclusive boundary", &TestIsSnapshotFreshZeroMaxAgeBoundary);
    run(L"GlobalMemoryStatusEx returns a valid snapshot", &TestQueryMemoryStatusReturnsValidSnapshot);
    run(L"plan memory clean none level", &TestPlanMemoryCleanNoneLevel);
    run(L"plan memory clean gates blocked", &TestPlanMemoryCleanGatesBlocked);
    run(L"plan memory clean above limit", &TestPlanMemoryCleanAboveLimit);
    run(L"refusing clean backend is honest", &TestRefusingCleanBackendIsHonest);
    run(L"clean plan orchestration", &TestCleanPlanOrchestration);
    return failed == 0 ? 0 : 1;
}
