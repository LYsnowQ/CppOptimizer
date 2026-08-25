#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace optimizer::policy {

// 资源余量分级。余量 = 可用资源 / 总量 * 100(百分比);
// v1 仅消费内存余量(available / total),CPU/GPU/磁盘余量留后续切片。
enum class ResourcePressure {
    Comfortable, // 余量充足(margin > comfortable 阈值)
    Adequate,    // 余量一般(margin > adequate 阈值)
    Tight,       // 余量紧张(margin > tight 阈值)
    Critical     // 余量危急(margin <= tight 阈值)
};

// 分级名(纯查询,恒成功)。
[[nodiscard]] const wchar_t* PressureToString(ResourcePressure pressure) noexcept;

// 策略动作。v1 只读咨询切片中均为“建议”；PWR-002 起 SuggestPriorityBoost
// 由 PolicyExecutor 经 PriorityBooster 落地（门禁后执行），其余仍为咨询。
enum class PolicyAction {
    NoOp,                // 无动作
    Notify,              // 提示用户（危急仅提示，不执行）
    SuggestMemoryTune,   // 建议 Layer 2 内存维护（咨询，R2 无执行器）
    SuggestPriorityBoost // 建议 Layer 3 优先级提升（PWR-002 规则产生，门禁后执行）
};

// 动作名(纯查询,恒成功)。
[[nodiscard]] const wchar_t* ActionToString(PolicyAction action) noexcept;

// 分级阈值(对应配置 [policy] 节)。合法约束:
// 0 <= tight < adequate < comfortable <= 100。
struct PolicyThresholds {
    std::int32_t comfortableMarginPercent = 30;
    std::int32_t adequateMarginPercent = 15;
    std::int32_t tightMarginPercent = 5;
};

// 纯函数:可用/总量 -> 内存余量百分比(0..100,向下取整,64 位乘防溢出)。
// total == 0 或 available > total 属于无效指标,返回 Validation 错误
// (黄色不变量:无效指标不得触发动作)。
[[nodiscard]] common::Result<std::uint32_t> ComputeMemoryMarginPercent(
    std::uint64_t availableBytes, std::uint64_t totalBytes) noexcept;

// 纯函数:余量百分比 -> 压力分级(确定性,可单测)。
// - margin 越界(< 0 或 > 100)返回 Validation 错误;
// - 阈值不满足 0 <= tight < adequate < comfortable <= 100 返回 Validation 错误
//   (配置层已拦截,此处防御手工构造的非法阈值);
// - 边界语义:margin > comfortable -> Comfortable;margin > adequate -> Adequate;
//   margin > tight -> Tight;否则 Critical(如 margin == comfortable 属 Adequate)。
[[nodiscard]] common::Result<ResourcePressure> ClassifyPressure(
    std::int32_t marginPercent,
    const PolicyThresholds& thresholds) noexcept;

// 游戏焦点信息(由 ProcessWatcher 观测 + GameConfig 配置合并而来)。
struct GameFocus {
    std::string gameId;              // 命中的规则 id(无游戏时为空)
    bool running = false;            // 是否有规则命中的游戏在运行
    bool foreground = false;         // 该游戏是否前台窗口
    bool pauseWhenBackground = true; // 配置 [[games]].pause_when_background
};

// 策略输入(一次求值的全部输入;压力已分级、指标已校验,求值函数恒不失败)。
struct PolicyInput {
    ResourcePressure pressure = ResourcePressure::Comfortable;
    GameFocus game;
};

// 策略决策输出。v1 全部为只读咨询:
// - reasonCode 为机器可读原因码(如 "mem_tight"),reason 为可读说明;
// - ttlMs 为建议有效时长(= 防抖冷却期),仅动作决策非零,NoOp 为零;
// - 决策绝不触发任何系统修改(执行器在后续批次实现)。
struct PolicyDecision {
    PolicyAction action = PolicyAction::NoOp;
    std::string gameId;
    std::string reasonCode;
    std::string reason;
    std::chrono::milliseconds ttlMs{0};
};

// 纯函数：规则评估（确定性、无副作用、恒成功）。
// v1/PWR-002 规则（按优先级，自上而下命中即返回）：
// 1) 无游戏运行                    -> NoOp (no_game)
// 2) 压力 Critical                 -> Notify (mem_critical)：危急优先提示，不受后台暂停限制
// 3) 游戏后台且配置后台暂停          -> NoOp (game_background)
// 4) 压力 Tight                    -> SuggestMemoryTune (mem_tight)：Layer 2 咨询
// 5) 压力 Adequate/Comfortable 且前台 -> SuggestPriorityBoost (prio_boost)：Layer 3，
//     由 PolicyExecutor 经 PriorityBooster 落地（PWR-002）
// 6) 其余（后台未暂停 + 余量充足）   -> NoOp (mem_ok)：无证据不优化
[[nodiscard]] PolicyDecision EvaluatePolicy(const PolicyInput& input) noexcept;

// 防抖滤波器:状态切换需经过冷却期,防止决策抖动。
// Update 接受外部时钟以便确定性测试;首次调用立即生效(无历史基线)。
class HysteresisFilter {
public:
    explicit HysteresisFilter(std::chrono::milliseconds cooldown) noexcept;

    // 输入新状态,返回生效状态:与旧状态相同则原样返回;
    // 不同且距上次切换不足冷却期则保持旧状态(抑制切换);
    // 否则切换并更新切换计时。
    [[nodiscard]] bool Update(bool newState,
                              std::chrono::steady_clock::time_point now) noexcept;

    [[nodiscard]] bool State() const noexcept;
    void Reset() noexcept;

private:
    std::chrono::milliseconds cooldown_;
    bool state_ = false;
    std::chrono::steady_clock::time_point lastChange_{};
    bool hasState_ = false;
};

// 求值结果:决策 + 是否被防抖抑制。
struct PolicyEvaluation {
    PolicyDecision decision;
    bool suppressed = false; // true 时 decision 为上一次有效输出(保持防抖前状态)
};

// 策略求值器:组合纯规则评估 + 防抖。
// 防抖按"是否行动"(action != NoOp)布尔状态驱动;动作间切换
// (如 Notify -> SuggestMemoryTune,均为行动态)不属于状态变化,立即生效。
class PolicyEvaluator {
public:
    PolicyEvaluator(
        PolicyThresholds thresholds = {},
        std::chrono::milliseconds cooldown =
            std::chrono::milliseconds(5000)) noexcept;

    // 求值 + 防抖。suppressed=true 时 decision 为上一次有效输出。
    // 动作决策的 ttlMs 置为冷却期(建议有效时长)。
    [[nodiscard]] PolicyEvaluation Evaluate(
        const PolicyInput& input,
        std::chrono::steady_clock::time_point now) noexcept;

    // 清空防抖与历史,下一次求值按首次处理。
    void Reset() noexcept;

    // 当前是否处于"行动"态(上次生效决策 action != NoOp)。
    [[nodiscard]] bool IsActing() const noexcept;

private:
    PolicyThresholds thresholds_;
    std::chrono::milliseconds cooldown_;
    HysteresisFilter filter_;
    PolicyDecision lastDecision_;
    bool hasLast_ = false;
};

} // namespace optimizer::policy
