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

// Runtime capability probe only. This class intentionally exposes no system-changing
// operation yet; write APIs require a separate safety permit and experimental build flag.
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
