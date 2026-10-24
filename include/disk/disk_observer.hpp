#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace optimizer::disk {

// 单个卷的只读快照。
struct VolumeInfo {
    std::wstring rootPath;   // 例 L"C:\\"
    std::wstring fileSystem; // 例 L"NTFS"；取不到时为空
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
};

// 进程 I/O 计数（GetProcessIoCounters 的只读子集）。
struct ProcessIo {
    std::uint32_t processId = 0;
    std::wstring name;
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
    std::uint64_t totalBytes() const noexcept { return readBytes + writeBytes; }
};

// 可注入后端：真实实现走 GetLogicalDrives/GetDiskFreeSpaceEx 与 Toolhelp + GetProcessIoCounters。
class DiskBackend {
public:
    virtual ~DiskBackend() = default;

    [[nodiscard]] virtual common::Result<std::vector<VolumeInfo>> ListVolumes() = 0;

    // 只采集可打开的进程；读不到的进程跳过（不伪造 0 值语义）。
    [[nodiscard]] virtual common::Result<std::vector<ProcessIo>> ListProcessIo() = 0;
};

[[nodiscard]] DiskBackend& RealDiskBackend() noexcept;

// 卷使用率分级（纯函数）：按“已用百分比”分级。
//   total == 0 -> Unknown；>= 90 -> High；>= 75 -> Medium；否则 Low。
enum class VolumePressure { Unknown, Low, Medium, High };

[[nodiscard]] VolumePressure ClassifyVolumeUsage(std::uint64_t totalBytes,
                                                 std::uint64_t freeBytes) noexcept;
[[nodiscard]] const wchar_t* ToString(VolumePressure pressure) noexcept;

// 按总 I/O 字节数降序取前 n 个（纯函数，稳定排序），用于 CLI 摘要。
[[nodiscard]] std::vector<ProcessIo> TopByIo(const std::vector<ProcessIo>& processes,
                                             std::size_t count);

// 统一的文本行（CLI 与测试共用）。
[[nodiscard]] std::wstring DescribeVolume(const VolumeInfo& volume);
[[nodiscard]] std::wstring DescribeProcessIo(const ProcessIo& process);

} // namespace optimizer::disk
