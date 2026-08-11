#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace optimizer::memory {

// A single read-only observation of system-wide physical memory.
// Byte counts use uint64_t so the public contract is independent of Win32 typedefs.
struct MemoryStatus {
    std::uint64_t totalPhysicalBytes = 0;
    std::uint64_t availablePhysicalBytes = 0;
    std::uint64_t usedPhysicalBytes = 0;
    std::uint32_t memoryLoadPercent = 0;
    std::chrono::steady_clock::time_point sampledAt{};
};

// Validates raw values and derives usedPhysicalBytes. This pure function is the
// first learner-owned extension point: tests can exercise it without querying Windows.
[[nodiscard]] common::Result<MemoryStatus> BuildMemoryStatus(
    std::uint64_t totalPhysicalBytes,
    std::uint64_t availablePhysicalBytes,
    std::uint32_t memoryLoadPercent,
    std::chrono::steady_clock::time_point sampledAt);

// Performs one read-only GlobalMemoryStatusEx query. It creates no owned resource,
// requires no elevated privilege, starts no worker thread, and changes no system state.
[[nodiscard]] common::Result<MemoryStatus> QueryMemoryStatus();

// Display-only byte formatting: "0 B", "1023 B", "1.5 KiB", "3.8 GiB".
// Uses binary units (1 KiB = 1024 bytes). Rounding happens only in the returned
// string; the uint64_t byte value is never modified.
[[nodiscard]] std::wstring FormatBytes(std::uint64_t bytes);

// Pure freshness check for a snapshot timestamp. A snapshot is fresh while
// `now - sampledAt <= maxAge`; the boundary is inclusive. A timestamp from the
// future is not stale. maxAge must be non-negative.
[[nodiscard]] bool IsSnapshotFresh(
    std::chrono::steady_clock::time_point sampledAt,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration maxAge);

} // namespace optimizer::memory
