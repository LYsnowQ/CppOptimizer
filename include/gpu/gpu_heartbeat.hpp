#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace optimizer::gpu {

// 只读的显存预算读数（DXGI_QUERY_VIDEO_MEMORY_INFO 的子集）。
struct VideoMemoryInfo {
    std::uint64_t budgetBytes = 0;    // 系统允许本进程使用的预算
    std::uint64_t usageBytes = 0;     // 当前用量
    std::uint64_t availableBytes = 0; // 预算内可用
};

// 一个适配器的只读快照。
struct AdapterSnapshot {
    std::string name;                 // 适配器描述（UTF-8；取不到时为空）
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemory = 0;
    std::optional<VideoMemoryInfo> memory; // 不支持 QueryVideoMemoryInfo 时为空
};

// 可注入后端：真实实现走 DXGI；测试用 fake。
class GpuBackend {
public:
    virtual ~GpuBackend() = default;

    // 枚举适配器与显存读数。没有可用适配器时返回空 vector（不是错误）。
    [[nodiscard]] virtual common::Result<std::vector<AdapterSnapshot>> EnumerateAdapters() = 0;
};

[[nodiscard]] GpuBackend& RealGpuBackend() noexcept;

// 显存压力分级（纯函数，便于单测）：
//   usage / budget >= 0.9 -> High；>= 0.7 -> Medium；否则 Low；
//   budget 为 0（读数不可用）-> Unknown。
enum class MemoryPressure { Unknown, Low, Medium, High };

[[nodiscard]] MemoryPressure ClassifyPressure(const VideoMemoryInfo& info) noexcept;
[[nodiscard]] const wchar_t* ToString(MemoryPressure pressure) noexcept;

// 适配器快照的文本行（供 CLI 与测试复用，保证输出口径一致）。
[[nodiscard]] std::wstring DescribeAdapter(const AdapterSnapshot& adapter);

} // namespace optimizer::gpu
