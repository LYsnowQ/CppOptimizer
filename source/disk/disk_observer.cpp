#include "disk/disk_observer.hpp"

#include "common/unique_resource.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <sstream>

namespace optimizer::disk {

namespace {

class RealDiskBackendImpl final : public DiskBackend {
public:
    common::Result<std::vector<VolumeInfo>> ListVolumes() override {
        const DWORD mask = ::GetLogicalDrives();
        if (mask == 0) {
            return common::Result<std::vector<VolumeInfo>>::Failure(
                common::Error::FromWin32(::GetLastError(), "GetLogicalDrives"));
        }
        std::vector<VolumeInfo> volumes;
        for (int index = 0; index < 26; ++index) {
            if ((mask & (1u << index)) == 0) {
                continue;
            }
            wchar_t root[4] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
            // 只读卷：不可访问的驱动器（如空读卡器）跳过，不影响其它卷。
            if (::GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR) {
                continue;
            }
            ULARGE_INTEGER freeBytes{};
            ULARGE_INTEGER totalBytes{};
            if (::GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, nullptr) == FALSE) {
                continue;
            }
            VolumeInfo volume;
            volume.rootPath = root;
            wchar_t fsName[MAX_PATH]{};
            if (::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fsName,
                                        MAX_PATH) != FALSE) {
                volume.fileSystem = fsName;
            }
            volume.totalBytes = totalBytes.QuadPart;
            volume.freeBytes = freeBytes.QuadPart;
            volumes.push_back(std::move(volume));
        }
        return common::Result<std::vector<VolumeInfo>>::Success(std::move(volumes));
    }

    common::Result<std::vector<ProcessIo>> ListProcessIo() override {
        common::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) {
            return common::Result<std::vector<ProcessIo>>::Failure(
                common::Error::FromWin32(::GetLastError(), "CreateToolhelp32Snapshot"));
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::vector<ProcessIo> processes;
        if (::Process32FirstW(snapshot.Get(), &entry) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                return common::Result<std::vector<ProcessIo>>::Success({});
            }
            return common::Result<std::vector<ProcessIo>>::Failure(
                common::Error::FromWin32(error, "Process32FirstW"));
        }
        do {
            common::UniqueHandle process(::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process) {
                continue; // 打不开的进程跳过（权限不足属正常）
            }
            IO_COUNTERS counters{};
            if (::GetProcessIoCounters(process.Get(), &counters) == FALSE) {
                continue;
            }
            ProcessIo item;
            item.processId = entry.th32ProcessID;
            item.name = entry.szExeFile;
            item.readBytes = counters.ReadTransferCount;
            item.writeBytes = counters.WriteTransferCount;
            processes.push_back(std::move(item));
        } while (::Process32NextW(snapshot.Get(), &entry) != FALSE);
        return common::Result<std::vector<ProcessIo>>::Success(std::move(processes));
    }
};

} // namespace

DiskBackend& RealDiskBackend() noexcept {
    static RealDiskBackendImpl backend;
    return backend;
}

VolumePressure ClassifyVolumeUsage(std::uint64_t totalBytes,
                                   std::uint64_t freeBytes) noexcept {
    if (totalBytes == 0) {
        return VolumePressure::Unknown;
    }
    const std::uint64_t used = totalBytes > freeBytes ? totalBytes - freeBytes : 0;
    const std::uint64_t usedPercent = (used * 100) / totalBytes;
    if (usedPercent >= 90) {
        return VolumePressure::High;
    }
    if (usedPercent >= 75) {
        return VolumePressure::Medium;
    }
    return VolumePressure::Low;
}

const wchar_t* ToString(VolumePressure pressure) noexcept {
    switch (pressure) {
        case VolumePressure::Unknown:
            return L"Unknown";
        case VolumePressure::Low:
            return L"Low";
        case VolumePressure::Medium:
            return L"Medium";
        case VolumePressure::High:
            return L"High";
    }
    return L"Unknown";
}

std::vector<ProcessIo> TopByIo(const std::vector<ProcessIo>& processes,
                               std::size_t count) {
    std::vector<ProcessIo> sorted = processes;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const ProcessIo& left, const ProcessIo& right) {
                         return left.totalBytes() > right.totalBytes();
                     });
    if (sorted.size() > count) {
        sorted.resize(count);
    }
    return sorted;
}

std::wstring DescribeVolume(const VolumeInfo& volume) {
    std::wostringstream line;
    line << L"volume: " << volume.rootPath << L" fs="
         << (volume.fileSystem.empty() ? std::wstring(L"(unknown)") : volume.fileSystem)
         << L" total=" << volume.totalBytes << L" free=" << volume.freeBytes
         << L" pressure=" << ToString(ClassifyVolumeUsage(volume.totalBytes, volume.freeBytes));
    return line.str();
}

std::wstring DescribeProcessIo(const ProcessIo& process) {
    std::wostringstream line;
    line << L"    pid=" << process.processId << L" " << process.name
         << L" read=" << process.readBytes << L" write=" << process.writeBytes;
    return line.str();
}

} // namespace optimizer::disk
