#include "gpu/gpu_heartbeat.hpp"

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <sstream>

namespace optimizer::gpu {

namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string WideToUtf8(const wchar_t* text) noexcept {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int length = static_cast<int>(::wcslen(text));
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), needed,
                                              nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return out;
}

class RealGpuBackendImpl final : public GpuBackend {
public:
    common::Result<std::vector<AdapterSnapshot>> EnumerateAdapters() override {
        ComPtr<IDXGIFactory1> factory;
        const HRESULT created =
            ::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
        if (FAILED(created)) {
            // 没有 DXGI（极端精简环境）不是崩溃点：如实返回失败原因。
            return common::Result<std::vector<AdapterSnapshot>>::Failure(
                common::Error::FromHResult(static_cast<std::int32_t>(created), "CreateDXGIFactory1"));
        }

        std::vector<AdapterSnapshot> adapters;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (adapter == nullptr) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue; // 单个适配器读不到就跳过，不影响其它适配器
            }
            AdapterSnapshot snapshot;
            snapshot.name = WideToUtf8(desc.Description);
            snapshot.vendorId = desc.VendorId;
            snapshot.deviceId = desc.DeviceId;
            snapshot.dedicatedVideoMemory = desc.DedicatedVideoMemory;

            ComPtr<IDXGIAdapter3> adapter3;
            if (SUCCEEDED(adapter.As(&adapter3)) && adapter3 != nullptr) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                    VideoMemoryInfo memory;
                    memory.budgetBytes = info.Budget;
                    memory.usageBytes = info.CurrentUsage;
                    memory.availableBytes = info.AvailableForReservation;
                    snapshot.memory = memory;
                }
            }
            adapters.push_back(std::move(snapshot));
        }
        return common::Result<std::vector<AdapterSnapshot>>::Success(std::move(adapters));
    }
};

} // namespace

GpuBackend& RealGpuBackend() noexcept {
    static RealGpuBackendImpl backend;
    return backend;
}

MemoryPressure ClassifyPressure(const VideoMemoryInfo& info) noexcept {
    if (info.budgetBytes == 0) {
        return MemoryPressure::Unknown;
    }
    const auto usagePercent = (info.usageBytes * 100) / info.budgetBytes;
    if (usagePercent >= 90) {
        return MemoryPressure::High;
    }
    if (usagePercent >= 70) {
        return MemoryPressure::Medium;
    }
    return MemoryPressure::Low;
}

const wchar_t* ToString(MemoryPressure pressure) noexcept {
    switch (pressure) {
        case MemoryPressure::Unknown:
            return L"Unknown";
        case MemoryPressure::Low:
            return L"Low";
        case MemoryPressure::Medium:
            return L"Medium";
        case MemoryPressure::High:
            return L"High";
    }
    return L"Unknown";
}

std::wstring DescribeAdapter(const AdapterSnapshot& adapter) {
    std::wostringstream line;
    const std::wstring name = adapter.name.empty() ? std::wstring(L"(unnamed)") : std::wstring(adapter.name.begin(), adapter.name.end());
    line << L"adapter: " << name
         << L" vendor=0x" << std::hex << adapter.vendorId << L" device=0x" << adapter.deviceId
         << std::dec << L" vram=" << adapter.dedicatedVideoMemory;
    if (adapter.memory.has_value()) {
        line << L" budget=" << adapter.memory->budgetBytes
             << L" usage=" << adapter.memory->usageBytes
             << L" pressure=" << ToString(ClassifyPressure(*adapter.memory));
    } else {
        line << L" budget=unknown pressure=Unknown";
    }
    return line.str();
}

} // namespace optimizer::gpu
