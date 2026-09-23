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
// 内容：信封行 `CppOptimizerPowerSchemeSave/1` + `saved=<GUID>`。信封不符 = 不当作恢复依据。
inline constexpr std::string_view kSchemeSaveEnvelope =
    "CppOptimizerPowerSchemeSave/1";

// 纯函数：从文件内容解析保存的 GUID（信封/字段/形式任一不符 -> nullopt，不宽松转换）。
[[nodiscard]] std::optional<std::string> ParseSavedSchemeContent(
    std::string_view content) noexcept;

// 写入恢复依据（**临时文件 + 原子替换**：写坏或写一半不得让恢复路径读到不可信的 GUID）。
[[nodiscard]] common::Result<void> SaveSchemeGuid(
    const std::filesystem::path& path, std::string_view guid) noexcept;

// 读取恢复依据：无文件 / 信封不符 -> Success(nullopt)（“没有恢复依据”不是错误）；IO 失败如实 Failure。
[[nodiscard]] common::Result<std::optional<std::string>> ReadSavedSchemeGuid(
    const std::filesystem::path& path) noexcept;

} // namespace optimizer::power
