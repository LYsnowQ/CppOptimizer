#include "memory/working_set.hpp"

#include "common/unique_resource.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <string>
#include <utility>

namespace optimizer::memory {

namespace {

// 由 PROCESS_MEMORY_COUNTERS 填充读数（两处调用共用，避免字段口径漂移）。
WorkingSetReading ToReading(const PROCESS_MEMORY_COUNTERS& counters) noexcept {
    WorkingSetReading reading;
    reading.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    reading.peakWorkingSetBytes =
        static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    return reading;
}

} // namespace

common::Result<WorkingSetReading> QueryWorkingSet(
    std::uint32_t processId) noexcept {
    if (processId == 0) {
        return common::Result<WorkingSetReading>::Failure(
            common::Error::Validation("QueryWorkingSet", L"进程 ID 不能为 0"));
    }
    common::UniqueHandle process(::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId)));
    if (!process) {
        return common::Result<WorkingSetReading>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)"));
    }
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(process.Get(), &counters, sizeof(counters))) {
        return common::Result<WorkingSetReading>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "GetProcessMemoryInfo"));
    }
    return common::Result<WorkingSetReading>::Success(ToReading(counters));
}

common::Result<WorkingSetReading> QueryOwnWorkingSet() noexcept {
    // 伪句柄：不需要 OpenProcess，也不需要关闭。
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters,
                                sizeof(counters))) {
        return common::Result<WorkingSetReading>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "GetProcessMemoryInfo(own process)"));
    }
    return common::Result<WorkingSetReading>::Success(ToReading(counters));
}

common::Result<void> TrimOwnWorkingSet() noexcept {
    // EmptyWorkingSet：把本进程的工作集页尽量换出（R1，局部可逆）。
    // 作用目标固定为 GetCurrentProcess()——命令行不提供任何“修剪其它进程”的入口。
    if (!::EmptyWorkingSet(::GetCurrentProcess())) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "EmptyWorkingSet(own process)"));
    }
    return common::Result<void>::Success();
}

WorkingSetDelta MakeWorkingSetDelta(
    const std::optional<WorkingSetReading>& before,
    const std::optional<WorkingSetReading>& after) noexcept {
    WorkingSetDelta delta;
    if (before.has_value()) {
        delta.beforeBytes = before->workingSetBytes;
    }
    if (after.has_value()) {
        delta.afterBytes = after->workingSetBytes;
    }
    return delta;
}

std::optional<std::int64_t> DeltaBytes(const WorkingSetDelta& delta) noexcept {
    if (!delta.beforeBytes.has_value() || !delta.afterBytes.has_value()) {
        return std::nullopt; // 缺一侧就不给差值：不给“看起来精确”的伪证据
    }
    return static_cast<std::int64_t>(*delta.afterBytes) -
           static_cast<std::int64_t>(*delta.beforeBytes);
}

bool WorkingSetReduced(const WorkingSetDelta& delta) noexcept {
    const auto difference = DeltaBytes(delta);
    return difference.has_value() && *difference < 0;
}

std::string DescribeWorkingSetDelta(const WorkingSetDelta& delta) {
    std::string text = "working_set_before=";
    text += delta.beforeBytes.has_value()
                ? std::to_string(*delta.beforeBytes)
                : std::string("unknown");
    text += " working_set_after=";
    text += delta.afterBytes.has_value() ? std::to_string(*delta.afterBytes)
                                         : std::string("unknown");
    text += " delta=";
    const auto difference = DeltaBytes(delta);
    text += difference.has_value() ? std::to_string(*difference)
                                   : std::string("unknown");
    return text;
}

common::Result<void> SelfWorkingSetTrimBackend::Execute(CleanKind kind) {
    switch (kind) {
        case CleanKind::WorkingSetTrim:
            return TrimOwnWorkingSet();
        case CleanKind::StandbyListPurge:
            return common::Result<void>::Failure(common::Error::Unsupported(
                "SelfWorkingSetTrimBackend::Execute",
                L"Standby/Modified List 清理未实现（R3；且只在隔离环境显式开启时才允许执行）"));
        case CleanKind::SystemFileCacheTrim:
            return common::Result<void>::Failure(common::Error::Unsupported(
                "SelfWorkingSetTrimBackend::Execute",
                L"系统文件缓存修剪未实现（R3；且只在隔离环境显式开启时才允许执行）"));
    }
    return common::Result<void>::Failure(common::Error::Unsupported(
        "SelfWorkingSetTrimBackend::Execute", L"未知的清理步骤"));
}

} // namespace optimizer::memory
