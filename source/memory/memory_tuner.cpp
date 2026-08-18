#include "memory/memory_tuner.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <format>
#include <string_view>
#include <utility>

namespace optimizer::memory {

    common::Result<MemoryStatus> BuildMemoryStatus(
        std::uint64_t totalPhysicalBytes,
        std::uint64_t availablePhysicalBytes,
        std::uint32_t memoryLoadPercent,
        std::chrono::steady_clock::time_point sampledAt) {
        if (totalPhysicalBytes == 0) {
            return common::Result<MemoryStatus>::Failure(
                common::Error::Validation(
                    "BuildMemoryStatus.totalPhysicalBytes",
                    L"Total physical memory must be greater than zero"));
        }

        if (availablePhysicalBytes > totalPhysicalBytes) {
            return common::Result<MemoryStatus>::Failure(
                common::Error::Validation(
                    "BuildMemoryStatus.availablePhysicalBytes",
                    L"Available physical memory cannot exceed total physical memory"));
        }

        if (memoryLoadPercent > 100) {
            return common::Result<MemoryStatus>::Failure(
                common::Error::Validation(
                    "BuildMemoryStatus.memoryLoadPercent",
                    L"Memory load percent must be in the range 0 to 100"));
        }

        MemoryStatus status{};
        status.totalPhysicalBytes = totalPhysicalBytes;
        status.availablePhysicalBytes = availablePhysicalBytes;
        status.usedPhysicalBytes = totalPhysicalBytes - availablePhysicalBytes;
        status.memoryLoadPercent = memoryLoadPercent;
        status.sampledAt = sampledAt;
        return common::Result<MemoryStatus>::Success(std::move(status));
    }

    common::Result<MemoryStatus> QueryMemoryStatus() {
        MEMORYSTATUSEX nativeStatus{};
        nativeStatus.dwLength = sizeof(nativeStatus);

        if (!::GlobalMemoryStatusEx(&nativeStatus)) {
            const DWORD error = ::GetLastError();
            return common::Result<MemoryStatus>::Failure(
                common::Error::FromWin32(error, "GlobalMemoryStatusEx"));
        }

        return BuildMemoryStatus(
            nativeStatus.ullTotalPhys,
            nativeStatus.ullAvailPhys,
            nativeStatus.dwMemoryLoad,
            std::chrono::steady_clock::now());
    }

    namespace {

        // 纯整数计算 bytes/divisor 的一位小数。remainder < divisor 恒成立，
        // 故 remainder*10 不会溢出 uint64_t；round-half-up 后 tenths==10 进位。
        struct Tenths {
            std::uint64_t whole;
            std::uint32_t tenths; // 0..9
        };

        Tenths ScaleToTenths(std::uint64_t bytes, std::uint64_t divisor) {
            const std::uint64_t whole = bytes / divisor;
            const std::uint64_t remainder = bytes % divisor;
            std::uint64_t tenths = (remainder * 10 + divisor / 2) / divisor;
            if (tenths == 10) {
                return {whole + 1, 0};
            }
            return {whole, static_cast<std::uint32_t>(tenths)};
        }
    } // namespace

    std::wstring FormatBytes(std::uint64_t bytes) {
        static constexpr std::uint64_t kKiB = 1024;
        static constexpr std::uint64_t kMiB = kKiB * 1024;
        static constexpr std::uint64_t kGiB = kMiB * 1024;
        static constexpr std::uint64_t kTiB = kGiB * 1024;

        if (bytes < kKiB) {
            return std::format(L"{} B", bytes);
        }

        // 从大到小取第一个 bytes >= divisor 的单位。
        struct Unit {
            std::uint64_t divisor;
            std::wstring_view label;
        };

        static constexpr Unit kUnits[] = {
            {kTiB, L"TiB"},
            {kGiB, L"GiB"},
            {kMiB, L"MiB"},
            {kKiB, L"KiB"},
        };

        for (const auto& unit : kUnits) {
            if (bytes >= unit.divisor) {
                const Tenths value = ScaleToTenths(bytes, unit.divisor);
                return std::format(L"{}.{} {}", value.whole, value.tenths, unit.label);
            }
        }

        // 对 bytes >= kKiB 不可达；防御性兜底确保函数始终有返回值。
        return std::format(L"{} B", bytes);
    }

    bool IsSnapshotFresh(
        std::chrono::steady_clock::time_point sampledAt,
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::duration maxAge) {
        // 边界包含：恰好 maxAge 前采样的快照仍新鲜。
        // maxAge==0 合法：仅 now==sampledAt 时新鲜。
        // 未来时间戳使差值取负，永不判陈旧。
        return now - sampledAt <= maxAge;
    }

} // namespace optimizer::memory
