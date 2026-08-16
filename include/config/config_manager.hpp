#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::config {

// 应用运行模式（对应 config [application].mode）。
enum class RunMode {
    Observe,
    Balanced,
    Experimental
};

// 内存清理级别（对应 [memory].max_clean_level；无 realtime，红色禁止）。
enum class CleanLevel {
    None,
    Light
};

// 进程优先级上限（对应 [priority].max_level；无 realtime，红色禁止）。
enum class PriorityLevel {
    None,
    AboveNormal,
    High
};

// [layers] 节：分层开关（只读观测/维护/紧急执行）。
struct LayerConfig {
    bool monitoring = true;
    bool maintenance = false;
    bool emergency = false;
};

// [power] 节。switchPowerScheme 为 R2 危险开关，默认关闭。
struct PowerConfig {
    bool executionRequired = true;
    bool displayRequired = false;
    bool switchPowerScheme = false; // R2：默认关闭
};

// [priority] 节。maxLevel 上限无 realtime（红色禁止）。
struct PriorityConfig {
    bool enabled = false;
    PriorityLevel maxLevel = PriorityLevel::AboveNormal;
};

// [gpu_heartbeat] 节。R3 Experimental。
struct GpuHeartbeatConfig {
    bool enabled = false; // R3：默认关闭
    double maxMeasuredLoadPercent = 1.0;
};

// [scheduler] / [disk_cache] 节（Experimental / Planned）。
struct ToggleConfig {
    bool enabled = false;
};

// [[games]] 数组元素。每个游戏规则必须有稳定 id。
struct GameConfig {
    std::string id;
    std::string displayName;
    std::vector<std::string> processNames;
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
    bool scheduledCleanEnabled = false; // R2：默认关闭
    bool allowNativeWrite = false;      // R3：默认关闭
    CleanLevel maxCleanLevel = CleanLevel::Light;
};

// 不可变配置快照。读取后拷贝使用，运行期不变量：所有字段已通过校验，
// 危险开关保持默认或显式开启（见 LoadConfig 契约）。
struct ConfigSnapshot {
    std::int64_t version = 0;
    ApplicationConfig application;
    LoggingConfig logging;
    LayerConfig layers;
    PowerConfig power;
    PriorityConfig priority;
    MemoryConfig memory;
    GpuHeartbeatConfig gpuHeartbeat;
    ToggleConfig scheduler;
    ToggleConfig diskCache;
    std::vector<GameConfig> games;
};

// 从 TOML 文件加载配置。契约：
// - 文件不存在、TOML 语法错误、类型不匹配或值越界均返回对应错误域（不抛异常）；
// - 缺失的键使用默认值（失败安全：坏配置不导致程序崩溃）；
// - 危险开关（scheduledCleanEnabled / allowNativeWrite）仅在显式配置为 true
//   时开启，任何解析异常都回退到 false；
// - 返回的 ConfigSnapshot 为不可变拷贝，调用方可安全持有。
[[nodiscard]] common::Result<ConfigSnapshot> LoadConfig(std::wstring_view path);

// 纯校验：运行模式名（observe/balanced/experimental，大小写不敏感）。
[[nodiscard]] common::Result<RunMode> ParseRunMode(std::string_view name);

// 纯校验：清理级别名（none/light，大小写不敏感）。
[[nodiscard]] common::Result<CleanLevel> ParseCleanLevel(std::string_view name);

// 纯校验：优先级上限名（none/above_normal/high，大小写不敏感；无 realtime）。
[[nodiscard]] common::Result<PriorityLevel> ParsePriorityLevel(std::string_view name);

} // namespace optimizer::config
