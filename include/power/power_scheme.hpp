#pragma once

#include "common/error.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
namespace optimizer::power {

// Windows 已知电源计划的 GUID（PowerGet/SetActiveScheme 用同一套 GUID）。
// 只作为**便捷别名**：命令行给出别名时先归一化再交给后端。
inline constexpr std::string_view kSchemeBalanced =
    "381B4222-F694-41F0-9685-FF5BB260DF2E";
inline constexpr std::string_view kSchemeHighPerformance =
    "8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C";
inline constexpr std::string_view kSchemePowerSaver =
    "A1841308-3541-4FAB-BC81-F71556F20B4A";

// 纯函数：把 GUID 文本归一化为**唯一比较口径**——去空白、去 `{}`、转大写，并校验
// 8-4-4-4-12 十六进制形式；非法（长度/分隔/非十六进制字符）-> nullopt（不做宽松转换）。
[[nodiscard]] std::optional<std::string> NormalizeSchemeGuid(
    std::string_view text) noexcept;

// 纯函数：把命令行别名（`balanced` / `high-performance` / `power-saver`，ASCII 大小写不敏感且
// 下划线等价横线）解析为 GUID；也接受直接给出的合法 GUID 文本；其它一律 nullopt。
[[nodiscard]] std::optional<std::string> ResolveSchemeArgument(
    std::string_view text) noexcept;

// 可注入后端：真实后端走 powrprof；测试用 fake（失败注入 / 记录调用）。
class PowerSchemeBackend {
public:
    virtual ~PowerSchemeBackend() = default;

    // 只读：当前活动电源计划的 GUID（已归一化）。
    [[nodiscard]] virtual common::Result<std::string> GetActive() = 0;

    // 切换活动电源计划（系统级、可逆；失败如实返回原始错误码，不静默重试）。
    [[nodiscard]] virtual common::Result<void> SetActive(
        std::string_view guid) = 0;
};

// 真实后端（PowerGetActiveScheme / PowerSetActiveScheme）。
[[nodiscard]] PowerSchemeBackend& Win32PowerSchemeBackend() noexcept;

// 切换结果（供审计/日记的 `state` 段）：保存的原 GUID + 目标 + 切换后**只读读回**的实际 GUID。
// `changed == false` 表示“调用没报错但读回与目标不一致”——必须如实上报（不得当成已切换）。
struct SchemeSwitchOutcome {
    std::string previousGuid; // 切换前保存的原 GUID（恢复依据）
    std::string targetGuid;   // 请求的目标 GUID
    std::string activeGuid;   // 切换后读回的实际 GUID
    bool changed = false;     // 读回结果 == 目标
};

// 切换（**先读原 GUID，后切换**）：读不到原 GUID 则**拒绝切换**——没有回滚信息不得改全局状态。
// 切换后必做只读读回校验；读回失败时返回 Failure（调用方应提示用 restore 回退）。
[[nodiscard]] common::Result<SchemeSwitchOutcome> ApplyScheme(
    PowerSchemeBackend& backend, std::string_view targetGuid);

// 恢复结果：切回保存值后的**只读读回**校验。
struct SchemeRestoreOutcome {
    std::string savedGuid;  // 保存的原始 GUID（恢复依据）
    std::string activeGuid; // 恢复后读回的实际 GUID
    bool restored = false;  // 读回结果 == 保存值
};
[[nodiscard]] common::Result<SchemeRestoreOutcome> RestoreScheme(
    PowerSchemeBackend& backend, std::string_view savedGuid);

// ---------- 恢复依据的持久化（每用户小文件） ----------
// 内容：信封行 `CppOptimizerPowerSchemeSave/1` + `state=pending|applied|restored` + `saved=<GUID>`
//（可选 `target=<GUID>`）。信封/状态/字段任一不符 -> 不接受（不宽松转换）。
// **状态机（崩溃恢复的依据）**：
//   pending  = 已写入回滚信息、**尚未确认切换生效**（切换前写入）-> 下次启动应**先恢复再工作**；
//   applied  = 切换已由读回校验确认（有意保留，等待显式 restore）；
//   restored = 已显式回滚完成。
inline constexpr std::string_view kSchemeSaveEnvelope =
    "CppOptimizerPowerSchemeSave/1";

enum class SchemeSaveState {
    Pending,
    Applied,
    Restored,
};

[[nodiscard]] const char* SchemeSaveStateToString(SchemeSaveState state) noexcept;
[[nodiscard]] std::optional<SchemeSaveState> ParseSchemeSaveState(
    std::string_view text) noexcept;

// 恢复依据记录（回滚目标 + 状态 + 可选目标）。
struct SavedSchemeRecord {
    std::string savedGuid;                 // 回滚目标（切换前的原 GUID）
    std::optional<std::string> targetGuid; // 本次想要切到的 GUID（仅作如实上报）
    SchemeSaveState state = SchemeSaveState::Pending;
};

// 纯函数：从文件内容解析记录（信封/状态/`saved=` 字段/形式任一不符 -> nullopt）。
[[nodiscard]] std::optional<SavedSchemeRecord> ParseSavedSchemeRecord(
    std::string_view content) noexcept;

// 写入记录（**临时文件 + 原子替换**：半写状态下的 GUID 比没有更危险）。
[[nodiscard]] common::Result<void> SaveSchemeRecord(
    const std::filesystem::path& path, const SavedSchemeRecord& record) noexcept;

// 读取记录：无文件 / 内容不可信 -> Success(nullopt)；IO 失败如实 Failure。
[[nodiscard]] common::Result<std::optional<SavedSchemeRecord>> ReadSavedSchemeRecord(
    const std::filesystem::path& path) noexcept;

// ---------- 崩溃恢复（纯函数判定 + 带记录的动作流） ----------
// 恢复决策（**纯函数**）：
// - 无记录 / applied / restored -> None（applied 是有意保留，不自动回滚）；
// - pending 且实际活动计划 **不等于** 保存值 -> RestoreToSaved（不确定是否切成功：回滚到已知的安全值）；
// - pending 且实际活动计划 **等于** 保存值 -> MarkNeverApplied（切换从未生效，只需把状态改成 restored）。
enum class SchemeRecoveryAction {
    None,
    RestoreToSaved,
    MarkNeverApplied,
};

[[nodiscard]] const char* SchemeRecoveryActionToString(
    SchemeRecoveryAction action) noexcept;

[[nodiscard]] SchemeRecoveryAction DecideSchemeRecovery(
    const std::optional<SavedSchemeRecord>& record,
    std::string_view activeGuid) noexcept;

// 切换（带记录）：**先写 pending 记录（含原 GUID）**，写不进去就**拒绝切换**；
// 切换后读回校验通过才把记录改为 applied（写不进去则如实上报：记录停在 pending，
// 下次启动会保守地回滚）。
struct SwitchFlowOutcome {
    std::string previousGuid;
    std::string targetGuid;
    std::string activeGuid;
    bool changed = false;              // 读回结果 == 目标
    bool recordMarkedApplied = false;  // applied 状态是否已落盘
};
[[nodiscard]] common::Result<SwitchFlowOutcome> ApplySchemeWithRecord(
    PowerSchemeBackend& backend, const std::filesystem::path& savePath,
    std::string_view targetGuid);

// 恢复（带记录）：读记录 -> 切回保存值 -> 读回校验 -> 成功后把记录改为 restored。
struct RestoreFlowOutcome {
    std::string savedGuid;
    std::string activeGuid;
    bool restored = false;
    bool recordMarkedRestored = false;
};
[[nodiscard]] common::Result<RestoreFlowOutcome> RestoreSchemeWithRecord(
    PowerSchemeBackend& backend, const std::filesystem::path& savePath);

// 启动/下一次动作时的“先恢复再工作”：按 DecideSchemeRecovery 执行。
struct SchemeRecoveryOutcome {
    SchemeRecoveryAction action = SchemeRecoveryAction::None;
    std::string savedGuid;
    std::string targetGuid; // pending 记录里声明的目标（可为空）
    std::string activeGuid; // 处理后的实际活动计划
    bool restored = false;  // 确实执行了回滚并读回校验通过
};
[[nodiscard]] common::Result<SchemeRecoveryOutcome> RecoverPendingScheme(
    PowerSchemeBackend& backend, const std::filesystem::path& savePath);

} // namespace optimizer::power
