#pragma once

#include "common/error.hpp"
#include "memory/memory_cleaner.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace optimizer::memory {

// 进程工作集读数（只读；单位字节，取自 PROCESS_MEMORY_COUNTERS）。
struct WorkingSetReading {
    std::uint64_t workingSetBytes = 0;
    std::uint64_t peakWorkingSetBytes = 0;
};

// 只读查询指定进程的工作集（PROCESS_QUERY_LIMITED_INFORMATION + GetProcessMemoryInfo）。
// 查询失败如实返回 Failure（**不返回全零读数冒充“已测到”**）。
[[nodiscard]] common::Result<WorkingSetReading> QueryWorkingSet(
    std::uint32_t processId) noexcept;

// 本进程工作集读数（pid = GetCurrentProcessId；不打开任何句柄）。
[[nodiscard]] common::Result<WorkingSetReading> QueryOwnWorkingSet() noexcept;

// 修剪**本进程**工作集（R1：作用域仅限当前进程，可逆——被换出的页会按需重新填充）。
// 单次调用、无循环、不提权、不触碰其它进程；失败如实返回（不伪装成功）。
[[nodiscard]] common::Result<void> TrimOwnWorkingSet() noexcept;

// 修剪前后读数（动作日记 `state` 段的可量化证据）。任一侧缺失时对应字段为空——
// **不把“没读到”写成 0**（否则会伪造出巨大的 delta）。
struct WorkingSetDelta {
    std::optional<std::uint64_t> beforeBytes;
    std::optional<std::uint64_t> afterBytes;
};

// 纯函数：由前后读数构造 delta（readings 为空表示该次读数失败）。
[[nodiscard]] WorkingSetDelta MakeWorkingSetDelta(
    const std::optional<WorkingSetReading>& before,
    const std::optional<WorkingSetReading>& after) noexcept;

// 纯函数：after - before（两侧都有读数时才有值；负值表示工作集下降）。
[[nodiscard]] std::optional<std::int64_t> DeltaBytes(
    const WorkingSetDelta& delta) noexcept;

// 纯函数：两侧都有读数且 after < before。
[[nodiscard]] bool WorkingSetReduced(const WorkingSetDelta& delta) noexcept;

// 纯函数：ASCII 单行描述（供日记/审计的 `state` 段），形如
// `working_set_before=123 working_set_after=45 delta=-78`；缺失一侧显示 `unknown`。
[[nodiscard]] std::string DescribeWorkingSetDelta(const WorkingSetDelta& delta);

// 清理后端：工作集修剪**真实执行但只作用于本进程**（R1）；
// Standby 列表清理 / 系统文件缓存修剪属 R3，本后端**如实拒绝**（Unsupported）——
// 它们需要真实的 Native 调用与隔离环境，未实现前绝不伪造成功。
class SelfWorkingSetTrimBackend final : public CleanBackend {
public:
    [[nodiscard]] common::Result<void> Execute(CleanKind kind) override;
};

} // namespace optimizer::memory
