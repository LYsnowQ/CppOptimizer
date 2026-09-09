#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::service {

// 宿主侧在场状态（常驻编排的在场度种子；仅用于观测/展示/记录，不参与 Safe Mode 触发——
// 触发清单范围以既有批准结论为准）。
enum class PresenceState {
    Unknown, // 无任何在场上报（无客户端或全部未知）
    Present, // 至少一个上报在场
    Away,    // 全部已知上报均不在场
};

// 状态名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* PresenceStateToString(PresenceState state) noexcept;

// 分类纯函数：距最近键鼠输入的秒数 < awayAfterSeconds -> Present；>= 阈值 -> Away；
// idle 缺失（Agent 未上报 user_idle_seconds）-> Unknown（不伪装在场）。
// awayAfterSeconds <= 0 视为不启用“不在场”判定：已知空闲一律 Present。
[[nodiscard]] PresenceState ClassifyPresence(
    const std::optional<std::uint32_t>& idleSeconds,
    std::uint32_t awayAfterSeconds) noexcept;

// 在场阈值纯函数：policyAwayIdleSeconds（[policy].user_away_idle_seconds）> 0 时采用之
//（与 ACT-004 政策“用户在场”判定口径一致），否则回退 fallback（默认 15 秒）。
// policy 为 0 = 政策侧不启用在场门禁，但展示侧仍需要一个非零阈值。
[[nodiscard]] std::uint32_t EffectivePresenceAwaySeconds(
    std::uint32_t policyAwayIdleSeconds,
    std::uint32_t fallback) noexcept;

// 单客户端在场账目（台账条目：最近一次上报的分类与空闲秒数）。
struct ClientPresence {
    std::string key;                    // 客户端键（如 pid:session）
    PresenceState state = PresenceState::Unknown;
    std::optional<std::uint32_t> idleSeconds; // 最近一次上报（未上报/解析失败为空）
};

// 宿主在场台账：按客户端汇总 Agent 周期上报（及宿主本机事件输入）的在场事实。
// 规则：同一客户端取最近一次上报；宿主 = 任一时点至少一个在场则 Present，否则任一 Away 则
// Away，否则 Unknown（“有人在”语义）。可选遗忘时长：超过即从台账移除。仅本进程内存状态，
// 不落盘。
// SVC-009：Summary() 在判定结果变化时触发一次 SetOnChange 注册的回调（在场度时间线事件），
// 供常驻编排记录/消费（只作观测，不做动作）。
// 线程模型：台账非线程安全——由单一拥有者线程（宿主 tick / 测试线程）调用。
class HostPresenceTracker {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    // 宿主汇总状态变化回调：Summary() 判定结果与上次不同时触发一次（previous/current 为
    // 变化前后状态）。在调用 Summary() 的线程执行；仅作观测/记录，不得阻塞或重入台账。
    using ChangeCallback =
        std::function<void(PresenceState previous, PresenceState current)>;

    struct Options {
        std::uint32_t awayAfterSeconds = 15;      // 空闲达此秒数视为不在场（0 = 不判定）
        std::chrono::seconds forgetAfter = std::chrono::seconds(0); // 0 = 不遗忘
        Clock now = [] { return std::chrono::steady_clock::now(); };
    };

    explicit HostPresenceTracker(Options options = {});

    // 注册汇总变化回调（SVC-009；可空 = 不回调）。覆盖先前回调。
    void SetOnChange(ChangeCallback callback) noexcept;

    // 记录一次上报（客户端键 + 可选空闲秒数）。返回该客户端本次分类（供日志标注）。
    [[nodiscard]] PresenceState Record(
        std::string_view clientKey,
        const std::optional<std::uint32_t>& idleSeconds) noexcept;

    // 宿主汇总（先做遗忘清理；判定变化时触发变化回调）。只读调用也可触发时间推进清理。
    [[nodiscard]] PresenceState Summary() noexcept;

    // 当前台账条目（按客户端键排序）。
    [[nodiscard]] std::vector<ClientPresence> Clients() noexcept;

    [[nodiscard]] std::size_t ClientCount() noexcept;

private:
    void PruneExpired() noexcept; // 移除超过遗忘时长的客户端
    // 只汇总（不更新 lastSummary_/不触发回调）——内部复用。
    [[nodiscard]] PresenceState ComputeSummary() const noexcept;

    Options options_;
    ChangeCallback onChange_;
    PresenceState lastSummary_ = PresenceState::Unknown;
    struct Entry {
        PresenceState state = PresenceState::Unknown;
        std::optional<std::uint32_t> idleSeconds;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace optimizer::service
