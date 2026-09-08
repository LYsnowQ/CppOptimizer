#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace optimizer::activity {

// 用户输入活动状态（MOD-ACT-001 首切片 + ACT-002 会话上下文，R0 只读）。
// Unknown=查询失败/不可交互（不伪装成活跃/空闲），Active=最近有输入，Idle=超过空闲阈值；
// Locked=工作站锁屏（会话连接仍在但输入被接管，输入时钟冻结），
// Disconnected=会话已断开（RDP 断开等，无交互可能）。Locked/Disconnected 仅由
// 会话上下文合并（ClassifyContextState/ObserveActivityContext）产生。
enum class ActivityState { Unknown, Active, Idle, Locked, Disconnected };

// 状态名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* ActivityStateToString(ActivityState state) noexcept;

// 一次最近输入查询结果。nowTick/lastInputTick 均为 GetTickCount 语义：毫秒、32 位、可回绕
//（约 49.7 天）；从未输入时 lastInputTick 为系统启动至今的 tick（GetLastInputInfo 语义，
// 本模块只在“有输入后”做相对判定，绝对 0 不作为“从未输入”的可靠信号处理）。
struct InputActivitySample {
    std::uint32_t nowTick = 0;       // 当前时刻 tick
    std::uint32_t lastInputTick = 0; // 最近键鼠输入时刻 tick
};

// 最近输入查询后端（可注入 fake 确定性测试；真实实现见 CreateWin32LastInputBackend）。
// 只读、无 Hook、不采集输入内容。
class LastInputBackend {
public:
    virtual ~LastInputBackend() = default;

    // 查询当前时刻与最近输入时刻。查询失败（如非交互会话/服务会话 0）返回 Failure，
    // 调用方按 Unknown 降级，不伪装成 Active/Idle。
    [[nodiscard]] virtual common::Result<InputActivitySample> Query() = 0;
};

// Win32 后端：GetTickCount + GetLastInputInfo（kernel32/user32 只读，无新库）。
[[nodiscard]] std::shared_ptr<LastInputBackend> CreateWin32LastInputBackend();

// GetTickCount 语义时间差（毫秒，处理 32 位回绕）：假定真实间隔 < 2^31 ms（约 24.8 天）；
// toTick 早于 fromTick（时钟回退/异源）按 0 返回（不做负差）。纯函数。
[[nodiscard]] std::int64_t TickDeltaMs(std::uint32_t fromTick,
                                       std::uint32_t toTick) noexcept;

// 距最近输入的空闲毫秒（now - lastInput；同 TickDeltaMs(lastInputTick, nowTick)）。
[[nodiscard]] std::int64_t IdleMilliseconds(
    std::uint32_t nowTick, std::uint32_t lastInputTick) noexcept;

// 空闲分类：idleMs >= idleThresholdMs -> Idle，否则 Active。阈值非负由调用方保证
//（负阈值退化为恒 Active）。纯函数。
[[nodiscard]] ActivityState ClassifyActivity(
    std::uint32_t nowTick, std::uint32_t lastInputTick,
    std::int64_t idleThresholdMs) noexcept;

// 观测窗口汇总（样本计数按状态归类）。
struct ActivityWindowSummary {
    std::size_t samples = 0;
    std::size_t active = 0;
    std::size_t idle = 0;
    std::size_t unknown = 0;
};

// ---------- ACT-002：会话/锁屏上下文（MOD-ACT-001 第二切片，R0 只读） ----------

// 会话连接状态（当前会话；值对应 WTS_CONNECTSTATE_CLASS，Unknown=不支持/未识别）。
// 只有 Active 对应“正常交互中”；Disconnected 指 RDP 等会话断开。
enum class SessionLinkState { Unknown, Active, Connected, Disconnected, Idle, Listen };

// 状态名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* SessionLinkStateToString(
    SessionLinkState state) noexcept;

// 纯映射：WTS_CONNECTSTATE_CLASS 整值 -> SessionLinkState
//（WTSConnectQuery/WTSShadow 等瞬时态按 Unknown，不做宽松解释）。
[[nodiscard]] SessionLinkState LinkStateFromWtsValue(int wtsConnectState) noexcept;

// 当前会话上下文快照（一次只读查询）。
// remoteSession=远程会话（RDP/Terminal Services），locked=工作站锁屏
//（WTSSessionInfoEx SessionFlags == WTS_SESSIONSTATE_LOCK，输入桌面被 Winlogon 接管），
// link=会话连接状态（WTSConnectState）。
struct SessionContext {
    bool remoteSession = false;
    bool locked = false;
    SessionLinkState link = SessionLinkState::Unknown;
};

// 会话上下文查询后端（可注入 fake 确定性测试；真实实现见 CreateWin32SessionProbe）。
// 只读 WTS 查询 + 系统指标，无订阅/无窗口/无后台线程。查询失败返回 Failure，
// 调用方整样本按 Unknown 降级（会话/锁屏未知时不得把“在场”伪装成已知）。
class SessionProbe {
public:
    virtual ~SessionProbe() = default;

    [[nodiscard]] virtual common::Result<SessionContext> Query() = 0;
};

// Win32 后端：WTSQuerySessionInformation（WTSConnectState + WTSSessionInfoEx
// SessionFlags）+ GetSystemMetrics(SM_REMOTESESSION)（wtsapi32/user32 只读，
// 无新窗口/线程/订阅）。当前会话标准用户可查询，失败即 Failure。
[[nodiscard]] std::shared_ptr<SessionProbe> CreateWin32SessionProbe();

// 合并输入活动与会话上下文为最终状态（纯函数，确定性）：
// 会话断开（link == Disconnected）-> Disconnected（断开即无交互）；
// 否则锁屏（locked）-> Locked（锁屏后输入时钟冻结，不得据此判 Active/Idle）；
// 否则原样返回输入状态（Active/Idle/Unknown）。remoteSession 不影响归类（仅展示）。
[[nodiscard]] ActivityState ClassifyContextState(
    ActivityState inputState, const SessionContext& context) noexcept;

// 会话上下文观测窗口汇总。
struct ActivityContextSummary {
    std::size_t samples = 0;
    std::size_t active = 0;
    std::size_t idle = 0;
    std::size_t locked = 0;
    std::size_t disconnected = 0;
    std::size_t unknown = 0;
};

// 会话上下文观测（ACT-002）：行为与 ObserveActivity 一致，但每样本额外查询一次会话
// 上下文并合并（断开/锁屏覆盖输入态）；输入查询与会话查询任一失败 -> 该样本 Unknown
// （不伪装）。sampleInterval 为 0 时不等待（确定性测试用）；sampleCount == 0 ->
// Validation 拒绝。onSample 收到 Locked/Disconnected 时 idleMs 为输入派生的原始值
//（锁屏/断开下意义有限，供展示参考）。
[[nodiscard]] common::Result<ActivityContextSummary> ObserveActivityContext(
    LastInputBackend& inputBackend, SessionProbe& sessionProbe,
    std::size_t sampleCount, std::chrono::milliseconds sampleInterval,
    std::int64_t idleThresholdMs,
    const std::function<void(ActivityState, std::int64_t)>& onSample);

// 前台有界观测：采样 sampleCount 次（间隔 sampleIntervalMs；为 0 时不等待，供确定性测试），
// 每次查询最近输入并分类；onSample(state, idleMs) 逐样本回调（CLI 打印等），查询失败样本回调
// Unknown 且计入 unknown（降级不伪装）。sampleCount == 0 -> Validation 拒绝。
[[nodiscard]] common::Result<ActivityWindowSummary> ObserveActivity(
    LastInputBackend& backend, std::size_t sampleCount,
    std::chrono::milliseconds sampleInterval, std::int64_t idleThresholdMs,
    const std::function<void(ActivityState, std::int64_t)>& onSample);

// ---------- ACT-003：前台窗口归属（MOD-ACT-001 第三切片，R0 只读） ----------

// 前台归属结果：当前 Active/Idle 样本所属前台窗口的进程。NotApplicable=状态不可归属
//（Locked/Disconnected/Unknown：锁屏时前台属安全桌面/断开无交互/失败不知在场，均不查询
// 前台、不伪造 pid）；Unknown=可归属但前台查询失败（降级不伪装）；NoWindow=可归属但当前
// 无前台窗口；Pid=可归属且前台窗口存在（pid 为其所属进程）。
enum class ForegroundAttribution { NotApplicable, Unknown, NoWindow, Pid };

// 前台窗口查询样本。hasWindow=false 表示当前桌面无前台窗口（安全桌面/无交互窗口等），
// 此时 pid 无意义；hasWindow=true 时 pid 为前台窗口所属进程（GetWindowThreadProcessId）。
struct ForegroundSample {
    bool hasWindow = false;
    std::uint32_t pid = 0;
};

// 前台窗口查询后端（可注入 fake 确定性测试；真实实现见 CreateWin32ForegroundProbe）。
// 只读 GetForegroundWindow + GetWindowThreadProcessId，无 Hook/无窗口/无消息循环/
// 无后台线程、不采集窗口内容（不读标题/类名）。查询失败返回 Failure，可归属样本按
// Unknown 降级不伪装 pid。
class ForegroundProbe {
public:
    virtual ~ForegroundProbe() = default;

    // 查询当前前台窗口的所属进程。无前台窗口（GetForegroundWindow 返回 NULL）不算失败，
    // 返回 Success{hasWindow=false}。
    [[nodiscard]] virtual common::Result<ForegroundSample> Query() = 0;
};

// Win32 后端：GetForegroundWindow + GetWindowThreadProcessId（user32 只读，无新库）。
// 窗口句柄在查询瞬间失效（进程已退出）视为失败如实上报。
[[nodiscard]] std::shared_ptr<ForegroundProbe> CreateWin32ForegroundProbe();

// 纯函数：状态可否做前台归属。仅 Active/Idle 可归属（用户可交互、窗口有归属意义）；
// Locked（前台属安全桌面）/Disconnected（无交互）/Unknown（不知在场）不可归属。
[[nodiscard]] bool IsForegroundAttributable(ActivityState state) noexcept;

// 纯函数：可归属状态下由前台窗口存在性得归属种类（hasWindow -> Pid，否则 NoWindow）。
[[nodiscard]] ForegroundAttribution ClassifyForegroundAttribution(
    bool hasWindow) noexcept;

// 前台归属样本（onSample 第三参）。attribution==Pid 时 pid 有效，其余恒 0。
struct ForegroundAttributionInfo {
    ForegroundAttribution attribution =
        ForegroundAttribution::NotApplicable;
    std::uint32_t pid = 0;
};

// 前台归属观测汇总（窗口级）。states 为状态计数（与 ObserveActivityContext 同语义；无会话
// 叠加时 locked/disconnected 恒 0），归属计数合计 == states.samples：
// attributedPid（Pid）+ noWindow（NoWindow）+ foregroundUnknown（Unknown）+
// notApplicable（NotApplicable）。
struct ActivityForegroundSummary {
    ActivityContextSummary states;
    std::size_t attributedPid = 0;      // Active/Idle 且前台窗口存在：pid 已知
    std::size_t noWindow = 0;           // Active/Idle 且当前无前台窗口
    std::size_t foregroundUnknown = 0;  // Active/Idle 但前台查询失败（不伪装 pid）
    std::size_t notApplicable = 0;      // Locked/Disconnected/Unknown（不可归属）
};

// 前台归属观测（ACT-003）：逐样本先查会话（sessionProbe != nullptr 时叠加，语义同
// ObserveActivityContext：会话或输入任一失败整样本 Unknown）再查输入，按
// ClassifyActivity/ClassifyContextState 得最终状态；仅 Active/Idle 样本查询一次前台归属
//（Locked/Disconnected/Unknown 不查询、计 notApplicable——不得伪造 pid），前台查询失败按
// Unknown 降级。Active/Idle 回调派生 idleMs；Locked/Disconnected 透传输入派生的 idleMs
//（展示参考）；Unknown 为 0。onSample(state, idleMs, attribution) 逐样本回调。
// sampleInterval 为 0 时不等待（确定性测试用）；sampleCount == 0 -> Validation 拒绝。
[[nodiscard]] common::Result<ActivityForegroundSummary> ObserveActivityForeground(
    LastInputBackend& inputBackend, SessionProbe* sessionProbe,
    ForegroundProbe& foregroundProbe, std::size_t sampleCount,
    std::chrono::milliseconds sampleInterval, std::int64_t idleThresholdMs,
    const std::function<void(ActivityState, std::int64_t,
                             const ForegroundAttributionInfo&)>& onSample);

} // namespace optimizer::activity
