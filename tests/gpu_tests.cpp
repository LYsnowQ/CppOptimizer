#include "gpu/gpu_heartbeat.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::Result;
using optimizer::gpu::AdapterSnapshot;
using optimizer::gpu::ClassifyPressure;
using optimizer::gpu::DescribeAdapter;
using optimizer::gpu::GpuBackend;
using optimizer::gpu::MemoryPressure;
using optimizer::gpu::VideoMemoryInfo;

class FakeGpuBackend final : public GpuBackend {
public:
    std::vector<AdapterSnapshot> adapters;
    bool fail = false;

    Result<std::vector<AdapterSnapshot>> EnumerateAdapters() override {
        if (fail) {
            return Result<std::vector<AdapterSnapshot>>::Failure(
                Error::Validation("EnumerateAdapters", L"模拟失败"));
        }
        return Result<std::vector<AdapterSnapshot>>::Success(adapters);
    }
};

VideoMemoryInfo MakeInfo(std::uint64_t budget, std::uint64_t usage) {
    VideoMemoryInfo info;
    info.budgetBytes = budget;
    info.usageBytes = usage;
    return info;
}

bool TestPressureThresholds() {
    return ClassifyPressure(MakeInfo(0, 0)) == MemoryPressure::Unknown &&
           ClassifyPressure(MakeInfo(1000, 0)) == MemoryPressure::Low &&
           ClassifyPressure(MakeInfo(1000, 699)) == MemoryPressure::Low &&
           ClassifyPressure(MakeInfo(1000, 700)) == MemoryPressure::Medium &&
           ClassifyPressure(MakeInfo(1000, 899)) == MemoryPressure::Medium &&
           ClassifyPressure(MakeInfo(1000, 900)) == MemoryPressure::High &&
           ClassifyPressure(MakeInfo(1000, 1000)) == MemoryPressure::High;
}

bool TestPressureNames() {
    return std::wstring(optimizer::gpu::ToString(MemoryPressure::Unknown)) == L"Unknown" &&
           std::wstring(optimizer::gpu::ToString(MemoryPressure::Low)) == L"Low" &&
           std::wstring(optimizer::gpu::ToString(MemoryPressure::Medium)) == L"Medium" &&
           std::wstring(optimizer::gpu::ToString(MemoryPressure::High)) == L"High";
}

bool TestDescribeWithMemory() {
    AdapterSnapshot adapter;
    adapter.name = "Test GPU";
    adapter.vendorId = 0x10de;
    adapter.deviceId = 0x1234;
    adapter.dedicatedVideoMemory = 8ull * 1024 * 1024 * 1024;
    adapter.memory = MakeInfo(1000, 950);
    const auto line = DescribeAdapter(adapter);
    return line.find(L"Test GPU") != std::wstring::npos &&
           line.find(L"budget=1000") != std::wstring::npos &&
           line.find(L"usage=950") != std::wstring::npos &&
           line.find(L"pressure=High") != std::wstring::npos;
}

bool TestDescribeWithoutMemory() {
    AdapterSnapshot adapter;
    adapter.name = "No Budget GPU";
    const auto line = DescribeAdapter(adapter);
    return line.find(L"budget=unknown") != std::wstring::npos &&
           line.find(L"pressure=Unknown") != std::wstring::npos;
}

bool TestFakeBackendEmpty() {
    FakeGpuBackend backend;
    const auto adapters = backend.EnumerateAdapters();
    return adapters && adapters.Value().empty();
}

bool TestFakeBackendFailure() {
    FakeGpuBackend backend;
    backend.fail = true;
    return !backend.EnumerateAdapters();
}

int g_failures = 0;

void Run(const wchar_t* name, bool (*test)()) {
    if (test()) {
        std::wcout << L"PASS " << name << L"\n";
        return;
    }
    ++g_failures;
    std::wcout << L"FAIL " << name << L"\n";
}

} // namespace

int wmain() {
    Run(L"pressure thresholds", &TestPressureThresholds);
    Run(L"pressure names", &TestPressureNames);
    Run(L"describe with memory", &TestDescribeWithMemory);
    Run(L"describe without memory", &TestDescribeWithoutMemory);
    Run(L"fake backend empty", &TestFakeBackendEmpty);
    Run(L"fake backend failure", &TestFakeBackendFailure);
    if (g_failures != 0) {
        std::wcout << g_failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"all gpu tests passed\n";
    return 0;
}
