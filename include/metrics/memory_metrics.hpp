#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace optimizer::metrics {

// 单个时刻的只读内存压力观测。loadPercent 对应 GlobalMemoryStatusEx.dwMemoryLoad
// （整数百分比 0..100）；availableBytes 对应 ullAvailPhys（原样保留字节数）。
struct MemorySample {
    std::uint32_t loadPercent = 0;
    std::uint64_t availableBytes = 0;
};

// 有界、非空观测窗口的整数报告。全程无浮点：平均负载对 0..100 值做 round-half-up，
// 运行和 <= 100*N 不会溢出；字节字段只做 min/max（无需求和），同样安全。
struct MemoryWindowReport {
    std::size_t sampleCount = 0;
    std::uint32_t minLoadPercent = 100; // 空窗口是错误，故初值有意义
    std::uint32_t maxLoadPercent = 0;
    std::uint32_t avgLoadPercent = 0;
    std::uint64_t minAvailableBytes = 0;
    std::uint64_t maxAvailableBytes = 0;
};

// 窗口聚合纯函数：顺序无关、无系统调用、无额外分配。契约：
// - 空窗口返回 Validation（零样本报告是伪装成功）；
// - 任一 loadPercent 须在 [0, 100]，否则 Validation。
[[nodiscard]] common::Result<MemoryWindowReport> AggregateMemoryWindow(
    std::span<const MemorySample> samples);

// 负载严格低于阈值（loadPercent < threshold，等于不计入）的样本占比（0..100），
// round-half-up、整数、顺序无关、无分配。契约：
// - threshold 须在 [0, 100]（0 合法恒为 0；100 仅排除满载样本）；
// - 空窗口或 threshold > 100 返回 Validation；
// - 溢出安全：count <= N 使 count*100 <= 100*N（同 AggregateMemoryWindow；
//   --observe 限制 N <= 60）。
[[nodiscard]] common::Result<std::uint32_t> ShareOfLoadBelow(
    std::span<const MemorySample> samples, std::uint32_t thresholdPercent);

} // namespace optimizer::metrics
