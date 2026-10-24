#include "disk/disk_observer.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::Result;
using optimizer::disk::ClassifyVolumeUsage;
using optimizer::disk::DescribeProcessIo;
using optimizer::disk::DescribeVolume;
using optimizer::disk::DiskBackend;
using optimizer::disk::ProcessIo;
using optimizer::disk::TopByIo;
using optimizer::disk::VolumeInfo;
using optimizer::disk::VolumePressure;

class FakeDiskBackend final : public DiskBackend {
public:
    std::vector<VolumeInfo> volumes;
    std::vector<ProcessIo> processes;
    bool fail = false;

    Result<std::vector<VolumeInfo>> ListVolumes() override {
        if (fail) {
            return Result<std::vector<VolumeInfo>>::Failure(
                Error::Validation("ListVolumes", L"模拟失败"));
        }
        return Result<std::vector<VolumeInfo>>::Success(volumes);
    }

    Result<std::vector<ProcessIo>> ListProcessIo() override {
        if (fail) {
            return Result<std::vector<ProcessIo>>::Failure(
                Error::Validation("ListProcessIo", L"模拟失败"));
        }
        return Result<std::vector<ProcessIo>>::Success(processes);
    }
};

bool TestVolumeThresholds() {
    return ClassifyVolumeUsage(0, 0) == VolumePressure::Unknown &&
           ClassifyVolumeUsage(1000, 1000) == VolumePressure::Low &&
           ClassifyVolumeUsage(1000, 251) == VolumePressure::Low &&
           ClassifyVolumeUsage(1000, 250) == VolumePressure::Medium &&
           ClassifyVolumeUsage(1000, 100) == VolumePressure::High &&
           ClassifyVolumeUsage(1000, 0) == VolumePressure::High;
}

bool TestVolumeFreeGreaterThanTotalIsLow() {
    // 异常读数（free > total）不得算出负使用率，按 Low 处理。
    return ClassifyVolumeUsage(1000, 2000) == VolumePressure::Low;
}

bool TestTopByIoOrdersAndCaps() {
    std::vector<ProcessIo> processes = {
        {1, L"a", 10, 0},
        {2, L"b", 100, 50},
        {3, L"c", 0, 5},
        {4, L"d", 0, 1},
    };
    const auto top = TopByIo(processes, 2);
    return top.size() == 2 && top[0].processId == 2 && top[1].processId == 1;
}

bool TestTopByIoEmpty() {
    return TopByIo({}, 3).empty();
}

bool TestDescribeVolume() {
    VolumeInfo volume;
    volume.rootPath = L"C:\\";
    volume.fileSystem = L"NTFS";
    volume.totalBytes = 1000;
    volume.freeBytes = 50;
    const auto line = DescribeVolume(volume);
    return line.find(L"C:\\") != std::wstring::npos && line.find(L"NTFS") != std::wstring::npos &&
           line.find(L"pressure=High") != std::wstring::npos;
}

bool TestDescribeProcessIo() {
    ProcessIo process{42, L"game.exe", 128, 256};
    const auto line = DescribeProcessIo(process);
    return line.find(L"pid=42") != std::wstring::npos &&
           line.find(L"game.exe") != std::wstring::npos &&
           line.find(L"read=128") != std::wstring::npos &&
           line.find(L"write=256") != std::wstring::npos;
}

bool TestFakeBackendFailure() {
    FakeDiskBackend backend;
    backend.fail = true;
    return !backend.ListVolumes() && !backend.ListProcessIo();
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
    Run(L"volume thresholds", &TestVolumeThresholds);
    Run(L"volume free>total", &TestVolumeFreeGreaterThanTotalIsLow);
    Run(L"top by io orders and caps", &TestTopByIoOrdersAndCaps);
    Run(L"top by io empty", &TestTopByIoEmpty);
    Run(L"describe volume", &TestDescribeVolume);
    Run(L"describe process io", &TestDescribeProcessIo);
    Run(L"fake backend failure", &TestFakeBackendFailure);
    if (g_failures != 0) {
        std::wcout << g_failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"all disk tests passed\n";
    return 0;
}
