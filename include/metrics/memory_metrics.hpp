#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace optimizer::metrics {

// A single read-only observation of system-wide memory pressure at one instant.
// loadPercent mirrors GlobalMemoryStatusEx.dwMemoryLoad (integer percent, 0..100).
// availableBytes mirrors ullAvailPhys (bytes, kept unchanged from the query).
struct MemorySample {
    std::uint32_t loadPercent = 0;
    std::uint64_t availableBytes = 0;
};

// Integer-only report over a bounded, non-empty observation window.
// No float anywhere: the average load is round-half-up over 0..100 values, so the
// running sum is <= 100 * sampleCount and cannot overflow for any bounded window.
// min/max need no summation at all, which is what keeps the bytes fields safe too.
struct MemoryWindowReport {
    std::size_t sampleCount = 0;
    std::uint32_t minLoadPercent = 100; // meaningful because an empty window is an error
    std::uint32_t maxLoadPercent = 0;
    std::uint32_t avgLoadPercent = 0;
    std::uint64_t minAvailableBytes = 0;
    std::uint64_t maxAvailableBytes = 0;
};

// Pure aggregation of a window of samples: order-independent, no system calls,
// no allocation beyond the result. An empty window is a Validation error: a
// zero-sample report would pretend to know what it does not (failure must not
// masquerade as success). Each loadPercent must be in [0, 100].
[[nodiscard]] common::Result<MemoryWindowReport> AggregateMemoryWindow(
    std::span<const MemorySample> samples);

// Percent (0..100) of samples whose loadPercent is strictly below
// thresholdPercent, rounded half-up, integer-only, order-independent, no
// allocation. thresholdPercent must be in [0, 100] (0 is legal: the share is
// then always 0 because loadPercent >= 0; 100 excludes only full-load samples).
// An empty window or a threshold above 100 is a Validation error: a zero-count
// denominator or an out-of-domain threshold must not masquerade as a result.
// Overflow-safe: count <= N makes count * 100 <= 100 * N, the same bound as the
// window load sum in AggregateMemoryWindow.
[[nodiscard]] common::Result<std::uint32_t> ShareOfLoadBelow(
    std::span<const MemorySample> samples, std::uint32_t thresholdPercent);

} // namespace optimizer::metrics
