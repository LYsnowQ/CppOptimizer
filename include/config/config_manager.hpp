#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::config {

// 应用运行模式。
enum class RunMode {
    Observe,
    Balanced,
    Experimental
};

// 内存清理级别。无 realtime。
enum class CleanLevel {
    None,
    Light
};

// 进程优先级上限。无 realtime。
enum class PriorityLevel {
    None,
    AboveNormal,
    High
};

// [layers] 节：分层开关。
struct LayerConfig {
    bool monitoring = true;
    bool maintenance = false;
    bool emergency = false;
};

// [power] 节。
struct PowerConfig {
    bool executionRequired = true;
    bool displayRequired = false;
    // switchPowerScheme 已解析未消费：R2 危险开关，全局电源计划切换待独立能力
    // （PowerSchemeController）实现后消费
    bool switchPowerScheme = false; // R2：默认关闭
};

// [priority] 节。maxLevel 上限无 realtime。
struct PriorityConfig {
    bool enabled = false;
    PriorityLevel maxLevel = PriorityLevel::AboveNormal;
};

// [gpu_heartbeat] 节。R3 Experimental。
struct GpuHeartbeatConfig {
    bool enabled = false; // R3：默认关闭
    double maxMeasuredLoadPercent = 1.0;
};

// [scheduler] / [disk_cache] 节。
struct ToggleConfig {
    bool enabled = false;
};

// [policy] 节：策略分级阈值与防抖冷却期。数值语义见 optimizer::policy。
// 合法约束：0 <= tight < adequate < comfortable <= 100，cooldown_ms >= 0；
// user_away_idle_seconds 为 0..86400（0 = 关闭用户在场门禁，保守默认）；
// halt_after_action_failures 为 0..10000（0 = 关闭 R1 动作连续失败停摆）。
// 违反约束属语义错误，LoadConfig 直接拒绝（错误阈值会产生错误决策）。
struct PolicyConfig {
    std::int32_t comfortableMarginPercent = 30;
    std::int32_t adequateMarginPercent = 15;
    std::int32_t tightMarginPercent = 5;
    std::int32_t cooldownMs = 5000;
    // 用户在场门禁（ACT-004）：0 = 关闭（默认，零回归）；>0 秒无键鼠输入视用户不在场
    //（AFK；锁屏/断开因输入时钟冻结自然落入），抑制优化建议（NoOp user_away）。
    std::int32_t userAwayIdleSeconds = 0;
    // R1 动作连续失败停摆（IPC-017）：0 = 关闭（默认，零回归）；>0 次连续 R1 动作失败
    //（优先级/电源获取或释放出错）后执行器停摆，不再调用后端（决策保持纯咨询）。
    std::int32_t haltAfterActionFailures = 0;
};

// [ipc] 节下 Safe Mode 门禁（Agent 受理侧的“身份/凭据失败异常”触发项）窗口参数。
// 缺省值须与 service::SafeModeGuard::Options 缺省一致（3 次 / 5000 ms / 2000 ms，
// enabled=true），宿主要暴露自定义值时应经此节读取而非改默认常量。
// 合法约束：1 <= failuresToEnter <= 100，
// 1 <= countingWindowMs <= 600000，1 <= cooldownMs <= 600000；
// 0 时长会使门禁无实际暂停效果（形同失效），语义上应以 enabled=false 显式关闭，
// 因此越界/为零配置属语义错误，LoadConfig 直接拒绝。
struct SafeModeConfig {
    bool enabled = true;
    std::int32_t failuresToEnter = 3;     // 时间窗口内失败阈值（次）
    std::int32_t countingWindowMs = 5000; // 计数窗口（滑动，窗口外失败过期）
    std::int32_t cooldownMs = 2000;       // 进入 Safe Mode 后的冷却期
};

// [ipc] 节：受保护管道/Agent 受理侧参数。当前仅 Safe Mode 门禁窗口参数。
struct IpcConfig {
    SafeModeConfig safeMode;
};

// [[games]] 数组元素。每个游戏规则必须有稳定 id。
struct GameConfig {
    std::string id;
    std::string displayName;
    std::vector<std::string> processNames;
    // 窗口标题子串过滤：非空时仅当进程主窗口标题含该子串才命中规则（辅助同名
    // 多实例/启动器与游戏同进程名场景）；空（默认）表示仅按进程名匹配。
    std::string windowTitleContains;
    bool pauseWhenBackground = true;
};


// [application] 节。
struct ApplicationConfig {
    RunMode mode = RunMode::Observe;
    bool safeModeOnRecoveryError = true;
};

// [logging] 节。
struct LoggingConfig {
    std::string level = "info";
    std::string directory;              // 空表示用默认目录
    std::uint32_t maxFileMb = 10;
    std::uint32_t maxFiles = 5;
    bool console = true;
};

// [memory] 节。危险开关默认 false，配置不得自动打开 R2/R3 能力。
struct MemoryConfig {
    bool queryEnabled = true;
    // scheduledCleanEnabled 已解析未消费：R2 危险开关，MemoryTuner 清理能力
    // （Experimental）门禁就绪后消费
    bool scheduledCleanEnabled = false; // R2：默认关闭
    // allowNativeWrite 已解析未消费：R3 预留，Native 写能力经安全评审后消费
    bool allowNativeWrite = false;      // R3：默认关闭
    CleanLevel maxCleanLevel = CleanLevel::Light;
};

// 不可变配置快照。读取后拷贝使用，运行期不变量：所有字段已通过校验，
// 危险开关保持默认或显式开启。
// 配置 schema 版本（三段数字 + 可选预发布标识，如 "1.0.0" / "1.2.0-beta.1"）。
// 主版本相同的配置均受支持（当前为 1）；其它主版本、非法形式、超长与非法字符均拒绝。
// 旧式整数（如 `version = 1`）按兼容读取并视为 "1.0.0"（既有配置零回归）。
struct ConfigVersion {
    std::uint32_t major = 1;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::string prerelease; // 不含前导 '-'（如 "beta.1"）；空 = 正式版
};

// 当前支持的配置 schema 主版本。
inline constexpr std::uint32_t kSupportedConfigMajor = 1;

// 解析与格式化（纯函数，可单测）：ParseConfigVersion 接受 "M.m.p[-pre]"；
// FormatConfigVersion 输出带预发布标识的规范文本（用于回显，保证 beta 等可显示）。
[[nodiscard]] common::Result<ConfigVersion> ParseConfigVersion(
    std::string_view text);
[[nodiscard]] std::string FormatConfigVersion(const ConfigVersion& version);

// [agent] 节：常驻 Agent 形态。
// **语义：声明的意图**——解析层受理 "startup_tray"（默认）/"task"/"service"，但**不触发任何注册动作**：
// 注册需要提权，必须由用户显式执行命令（一次性 UAC），配置永远不会自动注册或自动提权。
// 默认形态（启动项 + 托盘）仍是唯一默认值；"默认不可更改"由“未配置时恒回退默认值”保证。
struct AgentConfig {
    std::string form = "startup_tray"; // 唯一允许取值（默认）
};

struct ConfigSnapshot {
    // 配置 schema 版本：三段数字 + 可选预发布标识；主版本不受支持或格式非法时 LoadConfig 直接拒绝。
    ConfigVersion version;
    // 解析时遇到的**未知键**（点分路径，如 "logging.leval"、"totally_unknown_section"、
    // "games[0].nmae"）。未知键按“缺失的键使用默认值”原则被忽略以保持兼容，但必须
    // 如实上报——否则拼写错误会被静默吞掉（用户以为配置生效，实际用的是默认值）。
    std::vector<std::string> unknownKeys;
    ApplicationConfig application;
    LoggingConfig logging;
    LayerConfig layers;
    PowerConfig power;
    PriorityConfig priority;
    MemoryConfig memory;
    GpuHeartbeatConfig gpuHeartbeat;
    ToggleConfig scheduler;
    ToggleConfig diskCache;
    PolicyConfig policy;
    IpcConfig ipc;
    std::vector<GameConfig> games;
    AgentConfig agent;
};

// 从 TOML 文件加载配置。契约：
// - 文件不存在、TOML 语法错误、类型不匹配或值越界均返回对应错误域；
// - 缺失的键使用默认值；
// - 危险开关 scheduledCleanEnabled / allowNativeWrite 仅在显式配置为 true
//   时开启，任何解析异常都回退到 false；
// - 返回的 ConfigSnapshot 为不可变拷贝，调用方可安全持有。
[[nodiscard]] common::Result<ConfigSnapshot> LoadConfig(std::wstring_view path);

// 纯校验：运行模式名（observe/balanced/experimental，大小写不敏感）。
[[nodiscard]] common::Result<RunMode> ParseRunMode(std::string_view name);

// 能力门禁（纯函数，适合单测；保守默认：不明就拒）。
// 模式：observe 仅只读；balanced 允许 R1；experimental 可进入 R2/R3 门禁评估（真实动作仍须满足
// 全部安全条件（默认 fake/dry-run、逐条门禁、有界时长、可立即回滚、全程错误捕捉））。
// 层：monitoring 覆盖只读；maintenance 覆盖 R1/R2；emergency 覆盖 R3。
// 模式与层是**与**关系（都要允许才放行）：叠加门禁，避免单一开关被误认为“已获授权”。
[[nodiscard]] bool AllowsLocalReversibleActions(
    RunMode mode, const LayerConfig& layers) noexcept;
[[nodiscard]] bool AllowsSystemLevelActions(
    RunMode mode, const LayerConfig& layers) noexcept;

// 纯校验：清理级别名（none/light，大小写不敏感）。
[[nodiscard]] common::Result<CleanLevel> ParseCleanLevel(std::string_view name);

// 纯校验：优先级上限名（none/above_normal/high，大小写不敏感；无 realtime）。
[[nodiscard]] common::Result<PriorityLevel> ParsePriorityLevel(std::string_view name);

// ---------- 规则写入与合并（C 策略：config.local.toml 用户自建） ----------

// 纯函数：TOML 基本字符串转义（双引号/反斜杠/控制字符 -> 转义序列）。
// UTF-8 多字节字符原样保留。
[[nodiscard]] std::string ToTomlString(std::string_view text) noexcept;

// 纯函数：把游戏规则序列化为可追加的 TOML [[games]] 文本（含生成注释）。
// 输出为 UTF-8；追加在文件末尾时 TOML 数组跨多个 [[games]] 表头累积，语法合法。
[[nodiscard]] std::string FormatGameRulesToml(
    std::span<const GameConfig> rules) noexcept;

// 把规则追加到 TOML 文件末尾。原子写：先写临时文件再替换，
// 失败不破坏原文件；文件不存在时创建。追加不重写原内容（注释/格式保留）。
[[nodiscard]] common::Result<void> AppendGameRules(
    std::wstring_view path, std::span<const GameConfig> rules) noexcept;

// 纯函数：按 id（ASCII 大小写不敏感）合并两个游戏规则列表，
// local 覆盖 main 同 id 规则，其余追加在尾部。
[[nodiscard]] std::vector<GameConfig> MergeGameRules(
    std::span<const GameConfig> mainRules,
    std::span<const GameConfig> localRules) noexcept;

// 双路径加载：main 提供预设，local 提供用户自建规则（config.local.toml）。
// 合并语义（v1）：仅 [[games]] 参与合并（按 id 覆盖），其余节以 main 为准；
// local 文件不存在时返回 main 结果（不报错）；local 解析失败则报错（显式路径）。
[[nodiscard]] common::Result<ConfigSnapshot> LoadConfigWithLocal(
    std::wstring_view mainPath, std::wstring_view localPath) noexcept;

} // namespace optimizer::config
