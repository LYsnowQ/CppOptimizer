#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <mutex>

namespace optimizer::platform {

struct NativeCapabilities {
    bool ntdllLoaded = false;
    bool querySystemInformation = false;
    bool setSystemInformation = false;
    bool ntStatusConversion = false;
};

// 操作系统版本（RtlGetVersion 口径；Win10/11 均报 major=10/minor=0，build 为真实构建号）。
struct OsVersion {
    std::uint32_t majorVersion = 0;
    std::uint32_t minorVersion = 0;
    std::uint32_t buildNumber = 0;
};

// 操作系统支持判定（IPC-016 启动环境离散异常源）。支持矩阵（README：Windows 10/11 x64）：
// major == 10 且 build >= 10240（Win10 首个发布 1507），且原生架构 x64；其余视为不支持——
// Safe Mode 只允许 R0。Win10/11 Server 同内核（10.0/build>=14393）按受支持处理（R0 基线一致）。
enum class OsSupport {
    Supported,              // Win10/11 x64（10.0 build >= 10240）
    UnsupportedVersion,     // 版本低于 Win10 / 未来未识别 major / build < 10240
    UnsupportedArchitecture // 原生架构非 x64
};

// 操作系统支持判定（纯函数，可确定性单测）。
[[nodiscard]] OsSupport ClassifyOsSupport(const OsVersion& version,
                                          bool nativeX64) noexcept;

// RtlGetVersion 只读查询（动态取 ntdll 导出，无清单/版本助手语义依赖）。失败返回 Failure。
[[nodiscard]] common::Result<OsVersion> QueryOsVersion() noexcept;

// 当前进程原生架构是否为 x64（GetNativeSystemInfo，只读）。
[[nodiscard]] bool IsNativeX64() noexcept;

// 是否使用电池供电（GetSystemPowerStatus，只读）。
// 语义：`true` = 电池供电且未接交流电；`false` = 交流供电；
// 状态未知（API 失败或 ACLineStatus=255）返回 Failure——调用方不得把“未知”当作“安全”。
[[nodiscard]] common::Result<bool> QueryOnBatteryPower() noexcept;

// 仅运行时能力探测。本类有意不暴露任何改系统的操作；写 API 需另行安全许可与实验开关。
class NativeApi final {
public:
    static NativeApi& Instance() noexcept;

    [[nodiscard]] common::Result<NativeCapabilities> Probe() noexcept;

private:
    NativeApi() = default;
    NativeApi(const NativeApi&) = delete;
    NativeApi& operator=(const NativeApi&) = delete;

    void ProbeOnce() noexcept;

    NativeCapabilities capabilities_{};
    std::once_flag probeOnce_;
};

} // namespace optimizer::platform
