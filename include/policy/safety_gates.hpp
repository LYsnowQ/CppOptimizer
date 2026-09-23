#pragma once

#include "common/error.hpp"

#include <filesystem>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::policy {

// 危险能力的“六道门”（与危险操作策略的许可条件一致）：编译期开关、配置显式开启、
// 命令行显式确认、权限与环境门禁、审计可用、冷却满足。
// 本模块只做**判定与展示**：不执行任何系统动作，也不改变任何门禁状态。
enum class GateId {
    CompileTime,
    Config,
    CommandLine,
    PermissionAndEnvironment,
    Audit,
    Cooldown,
};

// 门名（ASCII，恒成功）：compile / config / cmdline / perm_env / audit / cooldown。
[[nodiscard]] const char* GateIdToString(GateId gate) noexcept;

// 逐门输入（true = 该门通过）。调用方负责把平台探测结果合并进 PermissionAndEnvironment。
struct GateInputs {
    bool compileTime = false;
    bool config = false;
    bool commandLine = false;
    bool permissionAndEnvironment = false;
    bool audit = false;
    bool cooldown = false;
};

// 判定结果：`allowed` 为六门全通过；`firstBlocking` 是**按固定顺序**遇到的第一个未通过门
// （顺序 = GateId 声明顺序），便于如实展示“先卡在哪一道”。
struct GateEvaluation {
    GateInputs gates{};
    bool allowed = false;
    std::optional<GateId> firstBlocking;
};

[[nodiscard]] GateEvaluation EvaluateGates(const GateInputs& inputs) noexcept;

// ---------- 编译期开关（compile 门） ----------
// 原则（2026-09-20 定）：**危险能力默认关**；只有构建时显式定义
// `OPTIMIZER_ENABLE_<能力>=1` 才视为“编译期已开启”。核心功能所需的能力不以配置形式出现，
// 而作为软件的**运行需求**（不在本表内）。
#ifndef OPTIMIZER_ENABLE_MEMORY_CLEAN
#define OPTIMIZER_ENABLE_MEMORY_CLEAN 0
#endif

// 各能力的编译期开关状态（默认恒为 false，与宏默认 0 一致）。
[[nodiscard]] constexpr bool MemoryCleanCompiledIn() noexcept {
    return OPTIMIZER_ENABLE_MEMORY_CLEAN != 0;
}

// ---------- 冷却门（cooldown） ----------
// 状态存**每用户文件**（默认 `%LOCALAPPDATA%\CppOptimizer\gate-cooldowns.txt`），
// 默认冷却 **15 分钟**（2026-09-20 定）；行格式：信封行 + `<capabilityId> <unix 秒>`。
inline constexpr std::string_view kCooldownEnvelope = "CppOptimizerCooldowns/1";
inline constexpr std::chrono::minutes kDefaultCooldown{15};

// 冷却台账：能力 ID -> 上次执行时刻（Unix 秒；0 = 无记录）。
struct CooldownLedger {
    std::map<std::string, std::int64_t, std::less<>> lastRunUnixSeconds;
};

// 内存清理能力的 ID（`--gates` 与 `--memory-clean` 的真实执行路径共用同一常量，
// 避免两处硬编码漂移导致“门禁显示放行、执行却按另一个 key 判定”）。
inline constexpr std::string_view kMemoryCleanCapabilityId = "memory.clean";

// 读取台账：文件不存在 = 无可记录（Success + 空台账，不是错误）；内容信封不符亦按空台账处理；
// 读取 IO 失败如实返回 Failure。
[[nodiscard]] common::Result<CooldownLedger> ReadCooldownLedger(
    const std::filesystem::path& path) noexcept;

// 写入台账（重写）：**临时文件 + 原子替换**（`MoveFileExW`），失败不破坏原文件
// （父目录自建；空路径拒绝；失败如实返回，不伪装已持久化）。
[[nodiscard]] common::Result<void> WriteCooldownLedger(
    const std::filesystem::path& path, const CooldownLedger& ledger) noexcept;

// 冷却门判定（**纯函数**）：无记录或已超出冷却窗口 -> true（可通过）；仍在窗口内 -> false。
[[nodiscard]] bool EvaluateCooldownGate(const CooldownLedger& ledger,
                                        std::string_view capabilityId,
                                        std::int64_t nowUnixSeconds,
                                        std::chrono::seconds cooldown) noexcept;

// 把一次执行并入台账（**纯函数**）：写入该能力的最新执行时刻；`capabilityId` 为空 -> 台账**不变**。
// **时钟回拨保护**：若台账已有**更晚**的时刻，则保留更晚者（不得因回拨把冷却窗口缩短或挪到过去）。
[[nodiscard]] CooldownLedger WithCooldownRun(const CooldownLedger& ledger,
                                             std::string_view capabilityId,
                                             std::int64_t nowUnixSeconds) noexcept;

// 记录一次**成功**执行（读 -> 改 -> 原子重写）。契约：
// - **只在动作成功后调用**（失败不得写：把“没做成”写成冷却会把后续重试错误地拦住）；
// - `capabilityId` 为空或路径为空 -> Validation 且**不触碰任何文件**；
// - 读取失败如实返回 Failure（**不得**用空台账覆盖已有记录）；写入失败如实返回。
[[nodiscard]] common::Result<void> RecordCooldownRun(
    const std::filesystem::path& path, std::string_view capabilityId,
    std::int64_t nowUnixSeconds) noexcept;

// 权限与环境门的事实输入（全部来自**只读探测**）。`factsKnown == false` 表示探测失败——
// 未知一律不得视为安全。
struct EnvironmentFacts {
    bool factsKnown = false;         // 全部事实是否取到
    bool osSupported = false;        // OS/build/x64 在支持矩阵内
    bool onBattery = false;          // 电池供电
    bool remoteSession = false;      // 远程会话（RDP）
    bool sessionLocked = false;      // 工作站锁屏
    bool interactiveSession = false; // 交互会话（非 Session 0）
};

// 权限与环境门判定（纯函数）：事实未知、OS 不支持、电池供电、远程会话、锁屏、
// 非交互会话——任一成立即**不得通过**（保守方向）。
[[nodiscard]] bool EvaluateEnvironmentGate(const EnvironmentFacts& facts) noexcept;

// 门禁报告（供机器可读输出）：字段均为 ASCII 令牌/布尔值，序列化无需转义。
struct GatesReportEntry {
    std::string name;          // 能力名（固定 ASCII 令牌）
    bool allowed = false;      // 六门全通
    std::string firstBlocking; // 首个阻塞门名；空 = 无（allowed 为真时）
};

struct GatesReport {
    bool readOnly = true;
    bool acknowledged = false;
    bool factsKnown = false;
    bool osSupported = false;
    bool onBattery = false;
    bool remoteSession = false;
    bool sessionLocked = false;
    bool auditWritable = false;
    std::vector<GatesReportEntry> capabilities;
};

// JSON 序列化（**纯函数**，单行输出；字段顺序固定，便于脚本与测试）。
// 契约：输出为合法 JSON 对象（无尾逗号）；`firstBlocking` 为空时输出 `null`。
[[nodiscard]] std::string FormatGatesJson(const GatesReport& report);

} // namespace optimizer::policy
