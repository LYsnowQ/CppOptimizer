#include "metrics/memory_metrics.hpp"

#include <algorithm>
#include <utility>

namespace optimizer::metrics {

    common::Result<MemoryWindowReport> AggregateMemoryWindow(
        std::span<const MemorySample> samples) {
        if (samples.empty()) {
            return common::Result<MemoryWindowReport>::Failure(
                common::Error::Validation(
                    "AggregateMemoryWindow.samples",
                    L"An observation window must contain at least one sample"));
        }

        for (const auto& sample : samples) {
            if (sample.loadPercent > 100) {
                return common::Result<MemoryWindowReport>::Failure(
                    common::Error::Validation(
                        "AggregateMemoryWindow.loadPercent",
                        L"Memory load percent must be in the range 0 to 100"));
            }
        }

        MemoryWindowReport report;
        report.sampleCount = samples.size();
        report.minAvailableBytes = samples.front().availableBytes;
        report.maxAvailableBytes = samples.front().availableBytes;

        // 平均负载对 0..100 有界域求和，sum <= 100*N 构造性无溢出；
        // 字节字段仅计算 min/max（无需求和），同样安全。
        std::uint64_t loadSum = 0;
        for (const auto& sample : samples) {
            report.minLoadPercent = std::min(report.minLoadPercent, sample.loadPercent);
            report.maxLoadPercent = std::max(report.maxLoadPercent, sample.loadPercent);
            loadSum += sample.loadPercent;
            report.minAvailableBytes = std::min(report.minAvailableBytes, sample.availableBytes);
            report.maxAvailableBytes = std::max(report.maxAvailableBytes, sample.availableBytes);
        }

        // Round-half-up 均值（与项目整数显示风格一致）。
        // loadSum + N/2 <= 100.5*N，远低于 uint64_t 上限。
        report.avgLoadPercent = static_cast<std::uint32_t>(
            (loadSum + report.sampleCount / 2) / report.sampleCount);
        return common::Result<MemoryWindowReport>::Success(std::move(report));
    }

    common::Result<std::uint32_t> ShareOfLoadBelow(
        std::span<const MemorySample> samples, std::uint32_t thresholdPercent) {
        if (samples.empty()) {
            return common::Result<std::uint32_t>::Failure(
                common::Error::Validation(
                    "ShareOfLoadBelow.samples",
                    L"An observation window must contain at least one sample"));
        }
        if (thresholdPercent > 100) {
            return common::Result<std::uint32_t>::Failure(
                common::Error::Validation(
                    "ShareOfLoadBelow.thresholdPercent",
                    L"Threshold must be in the range 0 to 100"));
        }

        // 严格小于：loadPercent == threshold 不计入。
        // count <= N 使 count*100 <= 100*N（与窗口负载和同界）；
        // (count*100 + N/2)/N 为 round-half-up。
        const auto count = static_cast<std::size_t>(std::count_if(
            samples.begin(), samples.end(),
            [thresholdPercent](const MemorySample& sample) {
                return sample.loadPercent < thresholdPercent;
            }));
        const auto percent =
            (count * 100 + samples.size() / 2) / samples.size();
        return common::Result<std::uint32_t>::Success(
            static_cast<std::uint32_t>(percent));
    }

} // namespace optimizer::metrics
