#pragma once

#include "common/error.hpp"
#include "config/config_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace optimizer::process {

// 进程生命周期状态。
// v1 只读观测阶段由 Toolhelp 轮询差分产生 NotRunning/Starting/Running/Exiting；
// Suspended 预留（线程挂起检测属 R2 高风险区），当前不会产生。
enum class ProcessState {
    NotRunning,
    Starting,
    Running,
    Suspended,
    Exiting
};

// 状态名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* StateToString(ProcessState state) noexcept;

// 一次进程生命周期信息。generation 与 pid 共同标识一次生命周期，
// 防止 PID 重用把先后两个不同进程误认为同一进程。
// 快照不暴露句柄：执行器按需以最小权限临时打开。
struct ProcessInfo {
    std::string gameId;                   // 命中的游戏规则 id（可能为空）
    std::uint32_t pid = 0;
    std::wstring processName;             // 进程可执行文件名，如 L"ExampleGame.exe"
    std::wstring windowTitle;             // 主窗口标题（无窗口时为空）
    ProcessState state = ProcessState::NotRunning;
    bool isForeground = false;
    bool isFullscreen = false;
    std::uint64_t generation = 0;         // 生命周期代数，从 1 起单调递增
    std::uint64_t creationTime100ns = 0;  // FILETIME 100ns 单位，0 表示未知
};

// 状态转移事件（一轮轮询产出的一个转移）。
struct ProcessTransition {
    std::string gameId;
    ProcessState previous = ProcessState::NotRunning;
    ProcessState current = ProcessState::NotRunning;
    ProcessInfo info;                     // 转移后的信息（Exiting 为最后已知信息）
};

// ---------- 纯函数（无 Windows 依赖，可单测） ----------

// 进程枚举条目（Toolhelp 快照的进程名 + pid）。
struct ProcessEntry {
    std::uint32_t pid = 0;
    std::wstring name;
};

// 游戏规则（宽字符串进程名形式）。
struct GameRule {
    std::string id;
    std::vector<std::wstring> processNames;
};

// 把 config::GameConfig（UTF-8 进程名）转为宽字符串规则。
// 非法 UTF-8 返回 Validation 错误。
[[nodiscard]] common::Result<std::vector<GameRule>> BuildGameRules(
    std::span<const config::GameConfig> games) noexcept;

// 进程名匹配：大小写不敏感（仅 ASCII 字母折叠，无区域依赖）；空规则名不匹配任何进程。
[[nodiscard]] bool ProcessNameMatches(
    std::wstring_view ruleName, std::wstring_view processName) noexcept;

// 规则匹配结果：每个规则至多一个条目（取枚举顺序首个匹配进程）。
struct RuleMatch {
    std::string gameId;
    ProcessEntry entry;
};

// 把进程枚举结果按规则匹配（规则内任一候选名命中即可）。
// 无匹配的规则不出现；同一 pid 可被多个规则命中。返回顺序与规则一致。
[[nodiscard]] std::vector<RuleMatch> MatchRulesToEntries(
    std::span<const GameRule> rules,
    std::span<const ProcessEntry> entries) noexcept;

// 当前轮次游戏在场摘要（含创建时间与窗口增强）。
struct GamePresence {
    std::string gameId;
    std::uint32_t pid = 0;
    std::wstring processName;
    std::uint64_t creationTime100ns = 0;
    std::wstring windowTitle;
    bool isForeground = false;
    bool isFullscreen = false;
};

// 差分结果：新的跟踪表 + 本轮状态转移。
struct DiffResult {
    std::vector<ProcessInfo> tracked;
    std::vector<ProcessTransition> transitions;
};

// 状态差分：把上一轮跟踪表与本轮 presence 合并，推导新跟踪表与转移。
// 同一性为 (gameId, pid)；pid 相同但创建时间不同（且都已知）视为重启：
// 旧生命周期发 Exiting，新生命周期发 Starting（代数递增）。
// nextGeneration 为 in/out 单调计数器，新生命周期分配当前值并自增。
[[nodiscard]] DiffResult DiffProcessPresence(
    std::span<const ProcessInfo> previous,
    std::span<const GamePresence> presence,
    std::uint64_t& nextGeneration) noexcept;

// ---------- Windows 只读封装 ----------

// 枚举系统全部进程（pid + 可执行文件名）。只读，不打开任何进程句柄。
[[nodiscard]] common::Result<std::vector<ProcessEntry>> EnumerateProcesses() noexcept;

// 查询进程创建时间（100ns FILETIME 单位）。权限不足或失败返回 0，按“未知”处理。
[[nodiscard]] std::uint64_t QueryProcessCreationTime(std::uint32_t pid) noexcept;

// 进程窗口信息。
struct WindowInfo {
    std::wstring title;           // 主窗口标题（无窗口时为空）
    bool isForeground = false;
    bool isFullscreen = false;
};

// 查询进程主窗口标题 / 前台 / 全屏状态。
// 窗口枚举失败（罕见）降级为空信息，不阻断观测（尽力而为）。
[[nodiscard]] common::Result<WindowInfo> QueryWindowInfo(std::uint32_t pid) noexcept;

// ---------- 进程观测器（可停止轮询线程） ----------

class ProcessWatcher {
public:
    struct Options {
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(1000);
        bool detectWindows = true;        // 窗口标题/前台/全屏检测
    };

    explicit ProcessWatcher(Options options = {}) noexcept;
    ~ProcessWatcher() noexcept;

    ProcessWatcher(const ProcessWatcher&) = delete;
    ProcessWatcher& operator=(const ProcessWatcher&) = delete;

    // 配置游戏规则（内部转宽字符串）。运行中调用会清空跟踪表与代数计数。
    [[nodiscard]] common::Result<void> SetRules(
        std::span<const config::GameConfig> games) noexcept;

    using ProcessEventCallback = std::function<void(const ProcessTransition&)>;

    // 订阅状态转移事件。callback 由轮询线程在锁外调用（黄色不变量），
    // 必须快速返回、不得抛异常（抛出会被捕获并忽略）、不得调用 Stop()。
    void Subscribe(ProcessEventCallback callback) noexcept;

    // 启动轮询线程；已运行时幂等成功。会清空跟踪表重新开始。
    [[nodiscard]] common::Result<void> Start() noexcept;

    // 停止并等待轮询线程退出；未运行时幂等。析构自动调用。
    void Stop() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

    // 当前跟踪快照（拷贝，只读）。
    [[nodiscard]] std::vector<ProcessInfo> GetTrackedProcesses() const noexcept;

    // 单次前台轮询：枚举 -> 匹配 -> 差分，返回本轮转移并更新跟踪表。
    // 不触发订阅回调（回调只由后台轮询线程触发）；供测试与单发场景使用。
    [[nodiscard]] common::Result<std::vector<ProcessTransition>> PollOnce() noexcept;

private:
    void WorkerLoop() noexcept;

    Options options_;
    mutable std::mutex mutex_;
    std::vector<GameRule> rules_;
    std::vector<ProcessInfo> tracked_;
    std::uint64_t nextGeneration_ = 1;
    std::vector<ProcessEventCallback> callbacks_;
    std::thread worker_;
    std::condition_variable stopCv_;
    bool stopRequested_ = false;
    bool running_ = false;
};

} // namespace optimizer::process
