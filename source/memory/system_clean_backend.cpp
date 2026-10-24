#include "memory/system_clean_backend.hpp"

#include "memory/working_set.hpp"

#include <windows.h>
#include <psapi.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace optimizer::memory {

namespace {

// NtSetSystemInformation 的声明形态（ntdll 导出，按需动态取址）。
using NtSetSystemInformationFn = long(__stdcall*)(unsigned long, void*, unsigned long);
// NtQuerySystemInformation 的声明形态（只读读数用）。
using NtQuerySystemInformationFn = long(__stdcall*)(unsigned long, void*, unsigned long,
                                                    unsigned long*);

// SYSTEM_MEMORY_LIST_INFORMATION（Win10/11 口径）：
// 5 个 SIZE_T + pageCountByPriority[8] + repurposedPagesByPriority[8] + 1 个 SIZE_T。
struct SystemMemoryListInformation {
    std::size_t zeroPageCount;
    std::size_t freePageCount;
    std::size_t modifiedPageCount;
    std::size_t modifiedNoWritePageCount;
    std::size_t badPageCount;
    std::size_t pageCountByPriority[8];
    std::size_t repurposedPagesByPriority[8];
    std::size_t modifiedPageCountPageFile;
};

// 特权名（winnt.h 的 SE_*_NAME 宏在部分 SDK/精简头配置下不可见，这里用字面量，
// 与 LookupPrivilegeValueW 的文档口径一致）。
constexpr wchar_t kSeProfileSingleProcess[] = L"SeProfileSingleProcessPrivilege";
constexpr wchar_t kSeIncreaseQuota[] = L"SeIncreaseQuotaPrivilege";

// SystemInformationClass = SystemMemoryListInformation（winternl/ntifs 口径；
// Win10/11 保持一致）。
constexpr unsigned long kSystemMemoryListInformation = 80;
// SYSTEM_MEMORY_LIST_COMMAND::MemoryPurgeStandbyList
constexpr unsigned long kMemoryPurgeStandbyList = 4;

[[nodiscard]] bool NtSuccess(long status) noexcept { return status >= 0; }

// 只读：当前令牌是否包含指定特权（不关心启用状态）。
[[nodiscard]] bool TokenHasPrivilege(const wchar_t* name) noexcept {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    LUID luid{};
    bool found = false;
    if (::LookupPrivilegeValueW(nullptr, name, &luid) != FALSE) {
        DWORD needed = 0;
        ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &needed);
        if (needed > 0) {
            std::unique_ptr<std::byte[]> buffer(new (std::nothrow) std::byte[needed]);
            if (buffer) {
                DWORD written = 0;
                if (::GetTokenInformation(token, TokenPrivileges, buffer.get(), needed,
                                          &written) != FALSE) {
                    const auto* privileges =
                        reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.get());
                    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
                        const LUID& candidate = privileges->Privileges[i].Luid;
                        if (candidate.LowPart == luid.LowPart &&
                            candidate.HighPart == luid.HighPart) {
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    ::CloseHandle(token);
    return found;
}

// 启用指定特权。注意 AdjustTokenPrivileges 在“未持有该特权”时也返回 TRUE，
// 必须再看 GetLastError() == ERROR_SUCCESS（否则会把“没这个权限”当成成功）。
[[nodiscard]] bool EnablePrivilege(const wchar_t* name) noexcept {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(),
                           TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    LUID luid{};
    bool enabled = false;
    if (::LookupPrivilegeValueW(nullptr, name, &luid) != FALSE) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const BOOL adjusted =
            ::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr,
                                    nullptr);
        enabled = (adjusted != FALSE) && (::GetLastError() == ERROR_SUCCESS);
    }
    ::CloseHandle(token);
    return enabled;
}

[[nodiscard]] common::Result<void> PurgeStandbyList() noexcept {
    if (!EnablePrivilege(kSeProfileSingleProcess)) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "PurgeStandbyList",
            L"未取得 SeProfileSingleProcessPrivilege（Standby 清理需要管理员权限）"));
    }
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "PurgeStandbyList", L"ntdll.dll 未加载，无法调用 NtSetSystemInformation"));
    }
    const auto setInformation = reinterpret_cast<NtSetSystemInformationFn>(
        ::GetProcAddress(ntdll, "NtSetSystemInformation"));
    if (setInformation == nullptr) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "PurgeStandbyList", L"ntdll 未导出 NtSetSystemInformation"));
    }
    // 单次调用、无循环：命令 = MemoryPurgeStandbyList。
    // 注意：清理 Standby 列表不可逆（页面只能等系统重新填充），因此仅在隔离环境执行。
    unsigned long command = kMemoryPurgeStandbyList;
    const long status =
        setInformation(kSystemMemoryListInformation, &command, sizeof(command));
    if (!NtSuccess(status)) {
        return common::Result<void>::Failure(common::Error::FromNtStatus(
            static_cast<std::int32_t>(status),
            "NtSetSystemInformation(SystemMemoryListInformation)"));
    }
    return common::Result<void>::Success();
}

// S4：系统文件缓存修剪。需要 SeIncreaseQuotaPrivilege。
// 语义：把文件缓存的上下限都置为 -1 会触发系统回收缓存页（页面只能按需重新读入，
// 不可逆，因此只在隔离环境执行）。单次调用，不做重试。
[[nodiscard]] common::Result<void> TrimSystemFileCache() noexcept {
    if (!EnablePrivilege(kSeIncreaseQuota)) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "TrimSystemFileCache",
            L"未取得 SeIncreaseQuotaPrivilege（文件缓存修剪需要管理员权限）"));
    }
    const SIZE_T noLimit = static_cast<SIZE_T>(-1);
    if (::SetSystemFileCacheSize(noLimit, noLimit, 0) == FALSE) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()), "SetSystemFileCacheSize"));
    }
    return common::Result<void>::Success();
}

} // namespace
std::optional<std::uint64_t> QuerySystemCacheBytes() noexcept {
    PERFORMANCE_INFORMATION info{};
    if (::GetPerformanceInfo(&info, sizeof(info)) == FALSE) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.SystemCache) *
           static_cast<std::uint64_t>(info.PageSize);
}
std::optional<std::uint64_t> QueryStandbyBytes() noexcept {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return std::nullopt;
    }
    const auto queryInformation = reinterpret_cast<NtQuerySystemInformationFn>(
        ::GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (queryInformation == nullptr) {
        return std::nullopt;
    }
    SystemMemoryListInformation info{};
    unsigned long returned = 0;
    const long status =
        queryInformation(kSystemMemoryListInformation, &info,
                         static_cast<unsigned long>(sizeof(info)), &returned);
    if (!NtSuccess(status) || returned != sizeof(info)) {
        // 读取失败或结构尺寸不符：返回 unknown，不给出可能错误的读数。
        return std::nullopt;
    }
    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    const std::uint64_t pageSize = systemInfo.dwPageSize;
    std::uint64_t pages = 0;
    for (const auto count : info.pageCountByPriority) {
        pages += static_cast<std::uint64_t>(count);
    }
    return pages * pageSize;
}

bool HasRequiredPrivilege(CleanKind kind) noexcept {
    switch (kind) {
        case CleanKind::WorkingSetTrim:
            return true; // 不需要特权；作用域仅本进程
        case CleanKind::StandbyListPurge:
            return TokenHasPrivilege(kSeProfileSingleProcess);
        case CleanKind::SystemFileCacheTrim:
            return TokenHasPrivilege(kSeIncreaseQuota);
    }
    return false;
}

common::Result<void> SystemMemoryCleanBackend::Execute(CleanKind kind) {
    switch (kind) {
        case CleanKind::WorkingSetTrim:
            return TrimOwnWorkingSet();
        case CleanKind::StandbyListPurge:
            return PurgeStandbyList();
        case CleanKind::SystemFileCacheTrim:
            return TrimSystemFileCache();
    }
    return common::Result<void>::Failure(common::Error::Unsupported(
        "SystemMemoryCleanBackend::Execute", L"未知的清理步骤"));
}

} // namespace optimizer::memory
