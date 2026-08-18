#pragma once

#include "common/error.hpp"

#include <chrono>

namespace optimizer::metrics {

// 单次 CPU 使用率采样结果。
// valid=false 表示 warming-up，首次采样无速率值；valid=true 时
// usagePercent 为 0..100 的当前使用率。
struct CpuSample {
    bool valid = false;
    double usagePercent = 0.0;
};

// PDH CPU 使用率查询。
// 封装 PdhOpenQueryW / PdhAddEnglishCounterW / PdhCollectQueryData /
// PdhGetFormattedCounterValue / PdhCloseQuery，计数器为
// "\Processor(_Total)\% Processor Time"，英文路径避免系统语言差异。
// 错误使用 PDH_STATUS，Error::FromPdh，不依赖 GetLastError。
//
// 采样节奏契约，速率计数器正确性的前提：
// % Processor Time 是速率型计数器，其值 = 两次采样间的平均占用，
// 采样间隔过短会得到失真的 0%/100% 随机值。因此本类在
// Sample() 内部保证两次采样间隔 >= minInterval，不足则前台等待补齐，
// 不引入后台线程，调用方无论怎样连续调用都能拿到有效速率值。
// 首次采样仍返回 warming-up，valid=false，第二次起为有效值。
class PdhCpuQuery {
public:
    explicit PdhCpuQuery(
        std::chrono::milliseconds minInterval = std::chrono::milliseconds(500)) noexcept;
    ~PdhCpuQuery() noexcept;

    PdhCpuQuery(const PdhCpuQuery&) = delete;
    PdhCpuQuery& operator=(const PdhCpuQuery&) = delete;

    // 打开查询并添加计数器。失败时对象保持关闭状态。
    [[nodiscard]] common::Result<void> Initialize() noexcept;

    // 单次采集。首次调用返回 valid=false 即 warming-up，第二次起返回有效值；
    // 两次有效采样之间保证 >= minInterval，不足则前台等待。
    // PDH 错误返回对应 Failure。
    [[nodiscard]] common::Result<CpuSample> Sample() noexcept;

    // 关闭查询句柄，幂等，析构自动调用。
    void Close() noexcept;

private:
    void* queryHandle_ = nullptr;   // HQUERY
    void* counterHandle_ = nullptr; // HCOUNTER
    bool hasPreviousSample_ = false;
    std::chrono::milliseconds minInterval_;
    std::chrono::steady_clock::time_point lastSampleTime_{};
    bool hasLastSampleTime_ = false;
};

} // namespace optimizer::metrics
