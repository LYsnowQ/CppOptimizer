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

        // sum <= 100 * sampleCount, so the average is overflow-safe by construction
        // for any bounded window; min/max never need a sum.
        std::uint64_t loadSum = 0;
        for (const auto& sample : samples) {
            report.minLoadPercent = std::min(report.minLoadPercent, sample.loadPercent);
            report.maxLoadPercent = std::max(report.maxLoadPercent, sample.loadPercent);
            loadSum += sample.loadPercent;
            report.minAvailableBytes = std::min(report.minAvailableBytes, sample.availableBytes);
            report.maxAvailableBytes = std::max(report.maxAvailableBytes, sample.availableBytes);
        }

        // Round-half-up mean, consistent with the project's integer display style.
        // loadSum + sampleCount / 2 <= 100.5 * sampleCount, far below uint64_t limits.
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

        // Strictly below: loadPercent == thresholdPercent is NOT counted.
        // count <= N, so count * 100 <= 100 * N, the same overflow bound as the
        // window load sum; (count * 100 + N / 2) / N is round-half-up.
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
