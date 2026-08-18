#include "metrics/pdh_metrics.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <chrono>
#include <string>
#include <thread>

namespace optimizer::metrics {

PdhCpuQuery::PdhCpuQuery(std::chrono::milliseconds minInterval) noexcept
    : minInterval_(minInterval) {}

PdhCpuQuery::~PdhCpuQuery() noexcept {
    Close();
}

common::Result<void> PdhCpuQuery::Initialize() noexcept {
    Close(); // 幂等：重复初始化前先关闭旧句柄

    HQUERY query = nullptr;
    PDH_STATUS status = ::PdhOpenQueryW(nullptr, 0, &query);
    if (status != ERROR_SUCCESS) {
        return common::Result<void>::Failure(
            common::Error::FromPdh(static_cast<std::int32_t>(status),
                                   "PdhOpenQueryW"));
    }

    // 英文计数器路径：避免系统显示语言导致路径失效。
    const wchar_t* kCounterPath = L"\\Processor(_Total)\\% Processor Time";
    HCOUNTER counter = nullptr;
    status = ::PdhAddEnglishCounterW(query, kCounterPath, 0, &counter);
    if (status != ERROR_SUCCESS) {
        ::PdhCloseQuery(query);
        return common::Result<void>::Failure(
            common::Error::FromPdh(static_cast<std::int32_t>(status),
                                   "PdhAddEnglishCounterW"));
    }

    queryHandle_ = query;
    counterHandle_ = counter;
    hasPreviousSample_ = false;
    hasLastSampleTime_ = false;
    return common::Result<void>::Success();
}

common::Result<CpuSample> PdhCpuQuery::Sample() noexcept {
    if (queryHandle_ == nullptr || counterHandle_ == nullptr) {
        return common::Result<CpuSample>::Failure(common::Error::Validation(
            "PdhCpuQuery::Sample", L"Query not initialized"));
    }

    // 采样节奏：速率计数器需要两次采样间有足够时间差才能算出有效速率。
    // 距上次采样不足 minInterval 时前台等待补齐。调用方连续调用也能拿到有效值，这是 API 契约。
    const auto now = std::chrono::steady_clock::now();
    if (hasLastSampleTime_) {
        const auto elapsed = now - lastSampleTime_;
        if (elapsed < minInterval_) {
            std::this_thread::sleep_for(minInterval_ - elapsed);
        }
    }

    PDH_STATUS status = ::PdhCollectQueryData(queryHandle_);
    if (status != ERROR_SUCCESS) {
        return common::Result<CpuSample>::Failure(
            common::Error::FromPdh(static_cast<std::int32_t>(status),
                                   "PdhCollectQueryData"));
    }

    lastSampleTime_ = std::chrono::steady_clock::now();
    hasLastSampleTime_ = true;

    // 速率型计数器首次采样无有效值：返回 warming-up。
    if (!hasPreviousSample_) {
        hasPreviousSample_ = true;
        return common::Result<CpuSample>::Success(CpuSample{false, 0.0});
    }

    PDH_FMT_COUNTERVALUE value{};
    status = ::PdhGetFormattedCounterValue(
        counterHandle_, PDH_FMT_DOUBLE, nullptr, &value);
    if (status != ERROR_SUCCESS) {
        return common::Result<CpuSample>::Failure(
            common::Error::FromPdh(static_cast<std::int32_t>(status),
                                   "PdhGetFormattedCounterValue"));
    }

    // 无效/未计算状态不冒充有效值。
    if (value.CStatus != PDH_CSTATUS_VALID_DATA &&
        value.CStatus != PDH_CSTATUS_NEW_DATA) {
        return common::Result<CpuSample>::Failure(common::Error::Validation(
            "PdhCpuQuery::Sample", L"Counter data not valid"));
    }

    CpuSample sample{true, value.doubleValue};
    return common::Result<CpuSample>::Success(sample);
}

void PdhCpuQuery::Close() noexcept {
    if (queryHandle_ != nullptr) {
        ::PdhCloseQuery(queryHandle_);
        queryHandle_ = nullptr;
        counterHandle_ = nullptr;
        hasPreviousSample_ = false;
        hasLastSampleTime_ = false;
    }
}

} // namespace optimizer::metrics
