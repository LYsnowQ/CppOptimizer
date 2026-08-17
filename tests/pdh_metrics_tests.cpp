#include "metrics/pdh_metrics.hpp"

#include <chrono>
#include <iostream>

namespace {

// 测试用小采样间隔，保证测试快速（真实默认 500ms 只影响 --cpu 命令）。
constexpr auto kTestInterval = std::chrono::milliseconds(10);

bool TestInitializeSucceeds() {
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    auto result = query.Initialize();
    return result.HasValue();
}

bool TestFirstSampleIsWarmingUp() {
    // 速率型计数器首次采样无有效值：必须返回 valid=false（不伪装成零值）。
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    if (!query.Initialize().HasValue()) {
        return false;
    }
    auto first = query.Sample();
    return first.HasValue() && !first.Value().valid;
}

bool TestSecondSampleIsValid() {
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    if (!query.Initialize().HasValue()) {
        return false;
    }
    auto first = query.Sample(); // warming-up
    auto second = query.Sample();
    if (!first.HasValue() || !second.HasValue()) {
        return false;
    }
    if (!second.Value().valid) {
        return false;
    }
    // CPU 使用率应在 [0, 100] 范围。
    return second.Value().usagePercent >= 0.0 &&
           second.Value().usagePercent <= 100.0;
}

bool TestSampleEnforcesInterval() {
    // 采样节奏契约：两次采样间隔必须 >= minInterval（不足则前台等待补齐），
    // 调用方连续调用也能拿到有效速率值。
    optimizer::metrics::PdhCpuQuery query(std::chrono::milliseconds(100));
    if (!query.Initialize().HasValue()) {
        return false;
    }
    auto first = query.Sample(); // warming-up，不等待
    auto second = query.Sample(); // 应等待补齐 100ms
    if (!first.HasValue() || !second.HasValue()) {
        return false;
    }
    const auto interval = std::chrono::milliseconds(100);
    const auto start = std::chrono::steady_clock::now();
    auto third = query.Sample();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!third.HasValue()) {
        return false;
    }
    // 第三次采样距第二次应 >= interval（100ms）。
    return elapsed >= interval && third.Value().valid;
}

bool TestSampleBeforeInitializeFails() {
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    auto result = query.Sample();
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestReinitializeIsIdempotent() {
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    if (!query.Initialize().HasValue()) {
        return false;
    }
    // 重复初始化应先关闭旧句柄再重开（无泄漏、无错误）。
    return query.Initialize().HasValue();
}

bool TestCloseIsIdempotent() {
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    if (!query.Initialize().HasValue()) {
        return false;
    }
    query.Close();
    query.Close(); // 幂等：重复关闭不崩溃
    // 关闭后采样应失败。
    auto result = query.Sample();
    return !result.HasValue();
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

    run(L"PDH query initializes", &TestInitializeSucceeds);
    run(L"First sample is warming up", &TestFirstSampleIsWarmingUp);
    run(L"Second sample is valid", &TestSecondSampleIsValid);
    run(L"Sample enforces interval contract", &TestSampleEnforcesInterval);
    run(L"Sample before init fails", &TestSampleBeforeInitializeFails);
    run(L"Re-initialize is idempotent", &TestReinitializeIsIdempotent);
    run(L"Close is idempotent", &TestCloseIsIdempotent);
    return failed == 0 ? 0 : 1;
}
