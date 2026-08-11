#include "platform/native_api.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace optimizer::platform {

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
        // GetModuleHandleW returns a borrowed module handle. It must not be freed.
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
