#pragma once

#include <optional>

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

} // namespace optimizer::policy
