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
// 触发清单范围以 docs/23 §6 与既有批准结论为准）。
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

// 单客户端在场账目（台账条目：最近一次上报的分类与空闲秒数）。
struct ClientPresence {
    std::string key;                    // 客户端键（如 pid 文本）
    PresenceState state = PresenceState::Unknown;
    std::optional<std::uint32_t> idleSeconds; // 最近一次上报（未上报/解析失败为空）
};

// 宿主在场台账：按客户端汇总 Agent 周期上报的在场事实。
// 规则：同一客户端取最近一次上报；宿主 = 任一时点至少一个在场则 Present，否则任一 Away 则
// Away，否则 Unknown（“有人在”语义：单个交互在场即视宿主在场）。可选遗忘时长：超过即从台账
// 移除（客户端停止上报后场状态不无限残留）。仅本进程内存状态，不落盘。
class HostPresenceTracker {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    struct Options {
        std::uint32_t awayAfterSeconds = 15;      // 空闲达此秒数视为不在场（0 = 不判定）
        std::chrono::seconds forgetAfter = std::chrono::seconds(0); // 0 = 不遗忘
        Clock now = [] { return std::chrono::steady_clock::now(); };
    };

    explicit HostPresenceTracker(Options options = {});

    // 记录一次上报（客户端键 + 可选空闲秒数）。返回该客户端本次分类（供日志标注）。
    [[nodiscard]] PresenceState Record(
        std::string_view clientKey,
        const std::optional<std::uint32_t>& idleSeconds) noexcept;

    // 宿主汇总（先做遗忘清理）。只读调用也可触发时间推进清理。
    [[nodiscard]] PresenceState Summary() noexcept;

    // 当前台账条目（按客户端键排序）。
    [[nodiscard]] std::vector<ClientPresence> Clients() noexcept;

    [[nodiscard]] std::size_t ClientCount() noexcept;

private:
    void PruneExpired() noexcept; // 移除超过遗忘时长的客户端

    Options options_;
    struct Entry {
        PresenceState state = PresenceState::Unknown;
        std::optional<std::uint32_t> idleSeconds;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace optimizer::service
