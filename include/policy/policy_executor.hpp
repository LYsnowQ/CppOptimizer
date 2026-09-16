#pragma once

#include "common/error.hpp"
#include "config/config_manager.hpp"
#include "policy/policy_engine.hpp"
#include "power/power_locker.hpp"
#include "priority/priority_booster.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace optimizer::policy {

// 一次 R1 动作尝试（AUD-001）：由执行器在每次实际调用后端（优先级/电源的获取、释放）后发出，
// 供调用方接入审计（本模块不依赖审计实现）。operationId 如 "priority.boost"；ok = 后端调用成功。
struct ExecutorActionEvent {
    std::string operationId;
    std::string target; // 目标描述（gameId/pid 等）
    std::string detail; // 参数/结果补充
    bool ok = true;
};

// 执行器配置（PWR-002 消费 [priority] 与 [power] 配置节）。
// 门禁语义：对应开关为 false 时该执行器不动作，决策仍由 PolicyEvaluator
// 产出并展示，仅不落地（默认全关 -> 纯咨询，与 POL-001 行为一致）。
struct ExecutorConfig {
    bool priorityEnabled = false;                 // [priority].enabled
    config::PriorityLevel priorityMaxLevel =
        config::PriorityLevel::AboveNormal;       // [priority].max_level
    bool powerExecutionRequired = false;          // [power].execution_required
    std::wstring powerReason = L"CppOptimizer policy: game running (R1)";
    // R1 动作连续失败停摆（IPC-017，执行器侧）：
    // >0 时 ApplyDecision 连续 N 次因 R1 动作失败（优先级/电源获取或释放出错）后执行器进入
    // halted——不再调用任何后端（决策仍由 PolicyEvaluator 产出展示，纯咨询），直到 ResetHalt。
    // 任何一次 ApplyDecision 成功（含无需动作）重置连续计数。0 = 关闭（缺省，零回归）。
    std::size_t consecutiveActionFailuresToHalt = 0; // [policy].halt_after_action_failures
    // 动作事件观察者（AUD-001，可选）：每次实际调用后端后回调一次（同步、调用线程）。
    // 用于审计接入；不得阻塞或抛异常（抛出由执行器捕获并忽略）。空 = 不回调（零回归）。
    std::function<void(const ExecutorActionEvent&)> actionObserver = nullptr;
};

// 游戏目标身份（由 ProcessWatcher 观测提供）。
// pid + 创建时间共同防 PID 重用；无效身份（pid == 0）不执行提升。
struct ExecutorTarget {
    std::string gameId;
    std::uint32_t pid = 0;
    std::uint64_t creationTime100ns = 0;
    bool running = false;
};

// 执行结果（每轮 ApplyDecision 的落地记录，供 CLI 输出与测试断言）。
struct ExecutorEffect {
    bool priorityBoosted = false;  // 本轮新提升
    bool priorityReleased = false; // 本轮释放提升
    bool powerHeld = false;        // 本轮新持有电源请求
    bool powerReleased = false;    // 本轮释放电源请求
    std::wstring skipped;          // 门禁/条件未满足时的原因（空表示无跳过）
};

// 策略执行器：把 PolicyEvaluator 的决策落地为 R1 执行器动作（PWR-002）。
// 契约（黄色不变量）：
// - 本类不重新评估决策，只消费 PolicyDecision；
// - Notify（危急）与 SuggestMemoryTune（R2 无执行器）不触发任何系统修改；
// - 执行以配置门禁为前提：[priority].enabled 且目标身份有效才提升，
//   [power].execution_required 且游戏运行才持有电源请求；
// - 期望状态对账：同目标重复 ApplyDecision 幂等（不重复获取），
//   目标变化（重启/换游戏）先释放旧动作再执行新动作；
// - 游戏退出或决策不再要求 -> 自动释放已持动作（游戏退出自动释放）；
// - 失败不伪装成功：获取/释放失败向上返回错误，不静默记为已执行。
class PolicyExecutor {
public:
    PolicyExecutor(std::shared_ptr<optimizer::power::PowerLocker> power,
                   std::shared_ptr<optimizer::priority::PriorityBooster>
                       priority,
                   ExecutorConfig config) noexcept;
    ~PolicyExecutor() noexcept;

    PolicyExecutor(const PolicyExecutor&) = delete;
    PolicyExecutor& operator=(const PolicyExecutor&) = delete;

    // 每轮决策后调用：把决策 + 目标身份与期望状态对齐，执行/释放 R1 动作。
    // 任一执行器失败即返回错误（成功部分保留，可重试补齐）。
    [[nodiscard]] common::Result<ExecutorEffect> ApplyDecision(
        const PolicyDecision& decision, const ExecutorTarget& target);

    // 释放全部已持动作（析构自动调用；进程退出时句柄随句柄表关闭为最终保障）。
    void ReleaseAll() noexcept;

    // 当前是否持有（供 CLI 输出/测试）。
    [[nodiscard]] bool IsPriorityHeld() const noexcept;
    [[nodiscard]] bool IsPowerHeld() const noexcept;

    // 是否处于 R1 动作连续失败停摆（IPC-017）。停摆期间 ApplyDecision 不再调用后端，
    // 返回 Success{skipped=说明}（决策保持纯咨询），直至 ResetHalt。
    [[nodiscard]] bool IsHalted() const noexcept;

    // 清除停摆并复位连续失败计数（下一次 ApplyDecision 按正常路径执行）。
    void ResetHalt() noexcept;

private:
    // 期望状态对账：优先级提升（决策要求 && 游戏运行 && 门禁开启）。
    [[nodiscard]] common::Result<void> ReconcilePriority(
        const PolicyDecision& decision, const ExecutorTarget& target,
        ExecutorEffect& effect);
    // 期望状态对账：游戏运行期电源请求（门禁开启）。
    [[nodiscard]] common::Result<void> ReconcilePower(
        const ExecutorTarget& target, ExecutorEffect& effect);
    // 发出动作事件（AUD-001；观察者空/抛异常均安全）。
    void EmitAction(std::string operationId, std::string target,
                    std::string detail, bool ok) noexcept;

    std::shared_ptr<optimizer::power::PowerLocker> power_;
    std::shared_ptr<optimizer::priority::PriorityBooster> priority_;
    ExecutorConfig config_;
    bool powerHeld_ = false;
    std::string priorityGameId_; // 当前已提升的 gameId（空 = 未持有）
    std::uint32_t priorityPid_ = 0;
    std::size_t consecutiveActionFailures_ = 0; // 连续 R1 动作失败计数（IPC-017）
    bool halted_ = false;                       // R1 动作停摆中（IPC-017）
};

} // namespace optimizer::policy
