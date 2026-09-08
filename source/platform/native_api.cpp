#include "platform/native_api.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace optimizer::platform {

// 与 RTL_OSVERSIONINFOW 同布局（避免依赖 winternl.h 的 SDK 差异）。
struct RtlOsVersionInfoW {
    ULONG cbSize = 0;
    ULONG major = 0;
    ULONG minor = 0;
    ULONG build = 0;
    ULONG platformId = 0;
    WCHAR csd[128]{};
};
using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfoW*);

OsSupport ClassifyOsSupport(const OsVersion& version, bool nativeX64) noexcept {
    if (!nativeX64) {
        return OsSupport::UnsupportedArchitecture;
    }
    if (version.majorVersion != 10 || version.minorVersion != 0 ||
        version.buildNumber < 10240) {
        return OsSupport::UnsupportedVersion;
    }
    return OsSupport::Supported;
}

common::Result<OsVersion> QueryOsVersion() noexcept {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return common::Result<OsVersion>::Failure(
            common::Error::Unsupported(
                "GetModuleHandleW(ntdll.dll)",
                L"ntdll.dll is not available in the current process"));
    }
    const auto getVersion = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(ntdll, "RtlGetVersion"));
    if (getVersion == nullptr) {
        return common::Result<OsVersion>::Failure(
            common::Error::Unsupported(
                "RtlGetVersion",
                L"RtlGetVersion is not exported by ntdll.dll"));
    }
    RtlOsVersionInfoW info{};
    info.cbSize = sizeof(info);
    const LONG status = getVersion(&info);
    if (status != 0) { // STATUS_SUCCESS
        return common::Result<OsVersion>::Failure(
            common::Error::FromNtStatus(status, "RtlGetVersion"));
    }
    OsVersion version;
    version.majorVersion = info.major;
    version.minorVersion = info.minor;
    version.buildNumber = info.build;
    return common::Result<OsVersion>::Success(version);
}

bool IsNativeX64() noexcept {
    SYSTEM_INFO info{};
    ::GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

NativeApi& NativeApi::Instance() noexcept {
    static NativeApi instance;
    return instance;
}

common::Result<NativeCapabilities> NativeApi::Probe() noexcept {
    ProbeOnce();
    if (!capabilities_.ntdllLoaded) {
        return common::Result<NativeCapabilities>::Failure(
            common::Error::Unsupported(
                "GetModuleHandleW(ntdll.dll)",
                L"ntdll.dll is not available in the current process"));
    }
    return common::Result<NativeCapabilities>::Success(capabilities_);
}

void NativeApi::ProbeOnce() noexcept {
    std::call_once(probeOnce_, [this]() noexcept {
        // GetModuleHandleW 返回借用模块句柄，不得释放。
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) {
            return;
        }

        capabilities_.ntdllLoaded = true;
        capabilities_.querySystemInformation =
            ::GetProcAddress(ntdll, "NtQuerySystemInformation") != nullptr;
        capabilities_.setSystemInformation =
            ::GetProcAddress(ntdll, "NtSetSystemInformation") != nullptr;
        capabilities_.ntStatusConversion =
            ::GetProcAddress(ntdll, "RtlNtStatusToDosError") != nullptr;
    });
}

} // namespace optimizer::platform
