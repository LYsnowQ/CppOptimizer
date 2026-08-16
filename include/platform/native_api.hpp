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
