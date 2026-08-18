#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace optimizer::memory {

// 系统物理内存的只读观测快照。字节数用 uint64_t，使公开契约不依赖 Win32 类型。
struct MemoryStatus {
    std::uint64_t totalPhysicalBytes = 0;
    std::uint64_t availablePhysicalBytes = 0;
    std::uint64_t usedPhysicalBytes = 0;
    std::uint32_t memoryLoadPercent = 0;
    std::chrono::steady_clock::time_point sampledAt{};
};

// 纯函数：校验原始值并派生 usedPhysicalBytes。契约：total>0、available<=total、
// load<=100，否则返回 Validation。测试无需查询 Windows 即可覆盖。
[[nodiscard]] common::Result<MemoryStatus> BuildMemoryStatus(
    std::uint64_t totalPhysicalBytes,
    std::uint64_t availablePhysicalBytes,
    std::uint32_t memoryLoadPercent,
    std::chrono::steady_clock::time_point sampledAt);

// 单次只读 GlobalMemoryStatusEx 查询：不创建资源、不需提权、不启动线程、不改系统状态。
[[nodiscard]] common::Result<MemoryStatus> QueryMemoryStatus();

// 仅显示用的字节格式化："0 B"/"1023 B"/"1.5 KiB"/"3.8 GiB"。
// 取整只发生在返回字符串中，uint64_t 真值永不被修改。
[[nodiscard]] std::wstring FormatBytes(std::uint64_t bytes);

// 快照时效纯函数：now - sampledAt <= maxAge 视为新鲜，边界包含；
// 未来时间戳不判陈旧；maxAge 必须非负。
[[nodiscard]] bool IsSnapshotFresh(
    std::chrono::steady_clock::time_point sampledAt,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration maxAge);

} // namespace optimizer::memory
