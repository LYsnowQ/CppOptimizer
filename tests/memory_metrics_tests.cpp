#include "metrics/memory_metrics.hpp"

#include <iostream>
#include <vector>

namespace {

bool TestAggregateRejectsEmptyWindow() {
    std::vector<optimizer::metrics::MemorySample> samples;
    auto result = optimizer::metrics::AggregateMemoryWindow(samples);
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestAggregateRejectsLoadAboveOneHundred() {
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{101, 1'000}};
    auto result = optimizer::metrics::AggregateMemoryWindow(samples);
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestAggregateSingleSample() {
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{42, 8'000}};
    auto result = optimizer::metrics::AggregateMemoryWindow(samples);
    return result.HasValue() &&
           result.Value().sampleCount == 1 &&
           result.Value().minLoadPercent == 42 &&
           result.Value().maxLoadPercent == 42 &&
           result.Value().avgLoadPercent == 42 &&
           result.Value().minAvailableBytes == 8'000 &&
           result.Value().maxAvailableBytes == 8'000;
}

bool TestAggregateMinMaxAvgOverThreeSamples() {
    // loads {40, 50, 60}: min 40, max 60, avg (150 / 3) = 50.
    // available {100, 90, 80}: min 80, max 100.
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{40, 100},
        optimizer::metrics::MemorySample{50, 90},
        optimizer::metrics::MemorySample{60, 80}};
    auto result = optimizer::metrics::AggregateMemoryWindow(samples);
    return result.HasValue() &&
           result.Value().sampleCount == 3 &&
           result.Value().minLoadPercent == 40 &&
           result.Value().maxLoadPercent == 60 &&
           result.Value().avgLoadPercent == 50 &&
           result.Value().minAvailableBytes == 80 &&
           result.Value().maxAvailableBytes == 100;
}

bool TestAggregateAverageRoundsHalfUp() {
    // {1, 2}: mean 1.5 rounds half-up to 2; {0, 1}: mean 0.5 rounds half-up to 1.
    std::vector<optimizer::metrics::MemorySample> first = {
        optimizer::metrics::MemorySample{1, 0},
        optimizer::metrics::MemorySample{2, 0}};
    std::vector<optimizer::metrics::MemorySample> second = {
        optimizer::metrics::MemorySample{0, 0},
        optimizer::metrics::MemorySample{1, 0}};
    auto a = optimizer::metrics::AggregateMemoryWindow(first);
    auto b = optimizer::metrics::AggregateMemoryWindow(second);
    return a.HasValue() && b.HasValue() &&
           a.Value().avgLoadPercent == 2 && b.Value().avgLoadPercent == 1;
}

bool TestAggregateIsOrderIndependent() {
    std::vector<optimizer::metrics::MemorySample> a = {
        optimizer::metrics::MemorySample{30, 5},
        optimizer::metrics::MemorySample{70, 9},
        optimizer::metrics::MemorySample{40, 7}};
    std::vector<optimizer::metrics::MemorySample> b = {
        optimizer::metrics::MemorySample{70, 9},
        optimizer::metrics::MemorySample{40, 7},
        optimizer::metrics::MemorySample{30, 5}};
    auto ra = optimizer::metrics::AggregateMemoryWindow(a);
    auto rb = optimizer::metrics::AggregateMemoryWindow(b);
    return ra.HasValue() && rb.HasValue() &&
           ra.Value().sampleCount == rb.Value().sampleCount &&
           ra.Value().minLoadPercent == rb.Value().minLoadPercent &&
           ra.Value().maxLoadPercent == rb.Value().maxLoadPercent &&
           ra.Value().avgLoadPercent == rb.Value().avgLoadPercent &&
           ra.Value().minAvailableBytes == rb.Value().minAvailableBytes &&
           ra.Value().maxAvailableBytes == rb.Value().maxAvailableBytes;
}

bool TestAggregateTenThousandFullLoadSamplesNoOverflow() {
    std::vector<optimizer::metrics::MemorySample> samples(
        10'000, optimizer::metrics::MemorySample{100, 0});
    auto result = optimizer::metrics::AggregateMemoryWindow(samples);
    return result.HasValue() &&
           result.Value().sampleCount == 10'000 &&
           result.Value().minLoadPercent == 100 &&
           result.Value().maxLoadPercent == 100 &&
           result.Value().avgLoadPercent == 100;
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

    run(L"Window rejects empty sample series", &TestAggregateRejectsEmptyWindow);
    run(L"Window rejects load above 100", &TestAggregateRejectsLoadAboveOneHundred);
    run(L"Window reports single sample", &TestAggregateSingleSample);
    run(L"Window min/max/avg over three samples", &TestAggregateMinMaxAvgOverThreeSamples);
    run(L"Window average rounds half up", &TestAggregateAverageRoundsHalfUp);
    run(L"Window aggregation is order independent", &TestAggregateIsOrderIndependent);
    run(L"Window 10000 full-load samples do not overflow", &TestAggregateTenThousandFullLoadSamplesNoOverflow);
    return failed == 0 ? 0 : 1;
}
