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
    // 负载 {40, 50, 60}: min 40, max 60, avg (150/3)=50。
    // 可用 {100, 90, 80}: min 80, max 100。
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
    // {1,2}: 均值 1.5 半向上取整为 2；{0,1}: 均值 0.5 取整为 1。
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

bool TestShareRejectsEmptyWindow() {
    std::vector<optimizer::metrics::MemorySample> samples;
    auto result = optimizer::metrics::ShareOfLoadBelow(samples, 50);
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestShareRejectsThresholdAboveOneHundred() {
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{10, 0}};
    auto result = optimizer::metrics::ShareOfLoadBelow(samples, 101);
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestShareAllBelowThreshold() {
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{10, 0},
        optimizer::metrics::MemorySample{20, 0},
        optimizer::metrics::MemorySample{30, 0}};
    auto result = optimizer::metrics::ShareOfLoadBelow(samples, 50);
    return result.HasValue() && result.Value() == 100;
}

bool TestShareEqualThresholdIsNotCounted() {
    // 严格小于：load == threshold 不计入，故 {40,50,60} 低于 50 只有 1 个(40)，
    // 低于 60 有 2 个(40,50)。
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{40, 0},
        optimizer::metrics::MemorySample{50, 0},
        optimizer::metrics::MemorySample{60, 0}};
    auto below50 = optimizer::metrics::ShareOfLoadBelow(samples, 50);
    auto below60 = optimizer::metrics::ShareOfLoadBelow(samples, 60);
    return below50.HasValue() && below60.HasValue() &&
           below50.Value() == 33 && below60.Value() == 67;
}

bool TestShareMixedWindow() {
    // {10,30,70,90} 低于 50: 4 个中 2 个 => 50%。
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{10, 0},
        optimizer::metrics::MemorySample{30, 0},
        optimizer::metrics::MemorySample{70, 0},
        optimizer::metrics::MemorySample{90, 0}};
    auto result = optimizer::metrics::ShareOfLoadBelow(samples, 50);
    return result.HasValue() && result.Value() == 50;
}

bool TestShareRoundsHalfUp() {
    // 3 个中 1 个低于 50 => 33；3 个中 2 个 => 67。
    std::vector<optimizer::metrics::MemorySample> oneOfThree = {
        optimizer::metrics::MemorySample{10, 0},
        optimizer::metrics::MemorySample{60, 0},
        optimizer::metrics::MemorySample{70, 0}};
    std::vector<optimizer::metrics::MemorySample> twoOfThree = {
        optimizer::metrics::MemorySample{10, 0},
        optimizer::metrics::MemorySample{20, 0},
        optimizer::metrics::MemorySample{70, 0}};
    auto a = optimizer::metrics::ShareOfLoadBelow(oneOfThree, 50);
    auto b = optimizer::metrics::ShareOfLoadBelow(twoOfThree, 50);
    return a.HasValue() && b.HasValue() && a.Value() == 33 && b.Value() == 67;
}

bool TestShareIsOrderIndependent() {
    std::vector<optimizer::metrics::MemorySample> a = {
        optimizer::metrics::MemorySample{10, 0},
        optimizer::metrics::MemorySample{70, 0},
        optimizer::metrics::MemorySample{40, 0}};
    std::vector<optimizer::metrics::MemorySample> b = {
        optimizer::metrics::MemorySample{70, 0},
        optimizer::metrics::MemorySample{40, 0},
        optimizer::metrics::MemorySample{10, 0}};
    auto ra = optimizer::metrics::ShareOfLoadBelow(a, 50);
    auto rb = optimizer::metrics::ShareOfLoadBelow(b, 50);
    return ra.HasValue() && rb.HasValue() && ra.Value() == rb.Value();
}

bool TestShareThresholdZeroAndFullLoadExclusion() {
    // threshold 0 合法：loadPercent >= 0 恒成立，严格小于 0 永假 => 占比恒 0。
    std::vector<optimizer::metrics::MemorySample> samples = {
        optimizer::metrics::MemorySample{0, 0},
        optimizer::metrics::MemorySample{50, 0}};
    auto atZero = optimizer::metrics::ShareOfLoadBelow(samples, 0);
    // threshold 100 仅排除满载样本：{0,100} 低于 100 为 1/2。
    std::vector<optimizer::metrics::MemorySample> fullLoad = {
        optimizer::metrics::MemorySample{0, 0},
        optimizer::metrics::MemorySample{100, 0}};
    auto atOneHundred = optimizer::metrics::ShareOfLoadBelow(fullLoad, 100);
    return atZero.HasValue() && atOneHundred.HasValue() &&
           atZero.Value() == 0 && atOneHundred.Value() == 50;
}

bool TestShareTenThousandSamplesNoOverflow() {
    std::vector<optimizer::metrics::MemorySample> samples(
        10'000, optimizer::metrics::MemorySample{40, 0});
    auto result = optimizer::metrics::ShareOfLoadBelow(samples, 50);
    return result.HasValue() && result.Value() == 100;
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
    run(L"Share rejects empty sample series", &TestShareRejectsEmptyWindow);
    run(L"Share rejects threshold above 100", &TestShareRejectsThresholdAboveOneHundred);
    run(L"Share all below threshold", &TestShareAllBelowThreshold);
    run(L"Share equal threshold is not counted", &TestShareEqualThresholdIsNotCounted);
    run(L"Share mixed window", &TestShareMixedWindow);
    run(L"Share rounds half up", &TestShareRoundsHalfUp);
    run(L"Share is order independent", &TestShareIsOrderIndependent);
    run(L"Share threshold 0 and full-load exclusion", &TestShareThresholdZeroAndFullLoadExclusion);
    run(L"Share 10000 samples do not overflow", &TestShareTenThousandSamplesNoOverflow);
    return failed == 0 ? 0 : 1;
}
