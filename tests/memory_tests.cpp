#include "memory/memory_tuner.hpp"

#include <chrono>
#include <iostream>

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
    return failed == 0 ? 0 : 1;
}
