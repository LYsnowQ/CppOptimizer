#include "metrics/pdh_metrics.hpp"

#include <chrono>
#include <iostream>

namespace {

// 测试用小采样间隔，保证测试快速。
constexpr auto kTestInterval = std::chrono::milliseconds(10);

bool TestInitializeSucceeds() {
    optimizer::metrics::PdhCpuQuery query(kTestInterval);
    auto result = query.Initialize();
    return result.HasValue();
}

bool TestFirstSampleIsWarmingUp() {
    // 速率型计数器首次采样无有效值：必须返回 valid=false。
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
    if (!first.HasValue()) {
        return false;
    }
    // PDH 速率计数器在活动系统上偶发返回无效/错误数据（计数器未就绪等），
    // 属正常抖动而非缺陷：重试直到拿到有效样本（有界），再校验取值范围。
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto sample = query.Sample();
        if (sample.HasValue() && sample.Value().valid) {
            // CPU 使用率应在 [0, 100] 范围。
            return sample.Value().usagePercent >= 0.0 &&
                   sample.Value().usagePercent <= 100.0;
        }
    }
    return false;
}

bool TestSampleEnforcesInterval() {
    // 采样节奏契约：两次采样间隔必须 >= minInterval，不足则前台等待补齐，
    // 调用方连续调用也能拿到有效速率值。
    // 契约保证的是两次采集之间隔，因此断言从第二次调用开始到第三次调用
    // 结束的总时长 >= interval：无论第二次采集本身耗时多少都成立，
    // 不受调度抖动影响（避免把慢采集误判为不等待）。
    // PDH 偶发无效数据时重试（有界），重试只会拉长总时长，不影响断言。
    optimizer::metrics::PdhCpuQuery query(std::chrono::milliseconds(100));
    if (!query.Initialize().HasValue()) {
        return false;
    }
    auto first = query.Sample(); // warming-up，不等待
    if (!first.HasValue()) {
        return false;
    }
    const auto interval = std::chrono::milliseconds(100);
    const auto start = std::chrono::steady_clock::now();
    bool sawValidPair = false;
    for (int attempt = 0; attempt < 20 && !sawValidPair; ++attempt) {
        auto second = query.Sample();
        auto third = query.Sample();
        if (second.HasValue() && third.HasValue() && third.Value().valid) {
            sawValidPair = true;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return sawValidPair && elapsed >= interval;
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
    // 重复初始化应先关闭旧句柄再重开。
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
