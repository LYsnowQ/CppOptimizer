#include "activity/user_activity.hpp"

#include <windows.h>
#include <wtsapi32.h>

#include <thread>

namespace optimizer::activity {

namespace {

// Win32 会话上下文后端：WTS 当前会话连接状态/锁屏标志 + 远程会话系统指标。
// 只读、无订阅/窗口/后台线程；当前会话标准用户可查询（失败如实返回 Failure）。
class Win32SessionProbe final : public SessionProbe {
public:
    common::Result<SessionContext> Query() override {
        SessionContext context;
        context.remoteSession = ::GetSystemMetrics(SM_REMOTESESSION) != 0;

        // 会话连接状态：WTSConnectState（WTS_CONNECTSTATE_CLASS）。
        LPWSTR buffer = nullptr;
        DWORD bytes = 0;
        if (!::WTSQuerySessionInformationW(
                WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                WTSConnectState, &buffer, &bytes)) {
            const DWORD code = ::GetLastError();
            if (buffer != nullptr) {
                ::WTSFreeMemory(buffer);
            }
            return common::Result<SessionContext>::Failure(
                common::Error::FromWin32(
                    code, "WTSQuerySessionInformationW(WTSConnectState)"));
        }
        if (bytes < sizeof(WTS_CONNECTSTATE_CLASS)) {
            ::WTSFreeMemory(buffer);
            return common::Result<SessionContext>::Failure(
                common::Error::Validation(
                    "Win32SessionProbe", L"WTSConnectState 返回过短"));
        }
        context.link = LinkStateFromWtsValue(static_cast<int>(
            *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(buffer)));
        ::WTSFreeMemory(buffer);

        // 锁屏标志：WTSSessionInfoEx（Level 1）SessionFlags。
        buffer = nullptr;
        bytes = 0;
        if (!::WTSQuerySessionInformationW(
                WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                WTSSessionInfoEx, &buffer, &bytes)) {
            const DWORD code = ::GetLastError();
            if (buffer != nullptr) {
                ::WTSFreeMemory(buffer);
            }
            return common::Result<SessionContext>::Failure(
                common::Error::FromWin32(
                    code, "WTSQuerySessionInformationW(WTSSessionInfoEx)"));
        }
        const auto* infoEx = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (bytes < sizeof(WTSINFOEXW) || infoEx->Level != 1) {
            ::WTSFreeMemory(buffer);
            return common::Result<SessionContext>::Failure(
                common::Error::Validation(
                    "Win32SessionProbe",
                    L"WTSSessionInfoEx 结构不支持（非 Level 1）"));
        }
        context.locked =
            infoEx->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        ::WTSFreeMemory(buffer);
        return common::Result<SessionContext>::Success(context);
    }
};

// Win32 最近输入后端：GetTickCount + GetLastInputInfo（只读、无 Hook、不采集输入内容）。
class Win32LastInputBackend final : public LastInputBackend {
public:
    common::Result<InputActivitySample> Query() override {
        LASTINPUTINFO info{};
        info.cbSize = sizeof(info);
        if (!::GetLastInputInfo(&info)) {
            return common::Result<InputActivitySample>::Failure(
                common::Error::FromWin32(::GetLastError(),
                                         "GetLastInputInfo"));
        }
        InputActivitySample sample;
        sample.nowTick = ::GetTickCount();
        sample.lastInputTick = info.dwTime;
        return common::Result<InputActivitySample>::Success(sample);
    }
};

} // namespace

const wchar_t* ActivityStateToString(ActivityState state) noexcept {
    switch (state) {
        case ActivityState::Unknown:
            return L"unknown";
        case ActivityState::Active:
            return L"active";
        case ActivityState::Idle:
            return L"idle";
        case ActivityState::Locked:
            return L"locked";
        case ActivityState::Disconnected:
            return L"disconnected";
    }
    return L"unknown";
}

const wchar_t* SessionLinkStateToString(SessionLinkState state) noexcept {
    switch (state) {
        case SessionLinkState::Unknown:
            return L"unknown";
        case SessionLinkState::Active:
            return L"active";
        case SessionLinkState::Connected:
            return L"connected";
        case SessionLinkState::Disconnected:
            return L"disconnected";
        case SessionLinkState::Idle:
            return L"idle";
        case SessionLinkState::Listen:
            return L"listen";
    }
    return L"unknown";
}

SessionLinkState LinkStateFromWtsValue(int wtsConnectState) noexcept {
    switch (wtsConnectState) {
        case WTSActive:
            return SessionLinkState::Active;
        case WTSConnected:
            return SessionLinkState::Connected;
        case WTSDisconnected:
            return SessionLinkState::Disconnected;
        case WTSIdle:
            return SessionLinkState::Idle;
        case WTSListen:
            return SessionLinkState::Listen;
        default: // WTSConnectQuery / WTSShadow / 未知值：瞬时/未识别态
            return SessionLinkState::Unknown;
    }
}

ActivityState ClassifyContextState(
    ActivityState inputState, const SessionContext& context) noexcept {
    // 断开优先：会话断开即无交互，锁屏标志在断开场景下无意义。
    if (context.link == SessionLinkState::Disconnected) {
        return ActivityState::Disconnected;
    }
    if (context.locked) {
        return ActivityState::Locked;
    }
    return inputState;
}

common::Result<ActivityContextSummary> ObserveActivityContext(
    LastInputBackend& inputBackend, SessionProbe& sessionProbe,
    std::size_t sampleCount, std::chrono::milliseconds sampleInterval,
    std::int64_t idleThresholdMs,
    const std::function<void(ActivityState, std::int64_t)>& onSample) {
    if (sampleCount == 0) {
        return common::Result<ActivityContextSummary>::Failure(
            common::Error::Validation(
                "ObserveActivityContext", L"sampleCount 必须为正"));
    }
    ActivityContextSummary summary;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const auto session = sessionProbe.Query();
        const auto input = inputBackend.Query();
        if (!session || !input) {
            // 会话或输入任一查询失败：整样本 Unknown（锁屏/断开未知时不得伪装）。
            ++summary.samples;
            ++summary.unknown;
            if (onSample) {
                onSample(ActivityState::Unknown, 0);
            }
        } else {
            const auto& sample = input.Value();
            const ActivityState inputState = ClassifyActivity(
                sample.nowTick, sample.lastInputTick, idleThresholdMs);
            const ActivityState state =
                ClassifyContextState(inputState, session.Value());
            const std::int64_t idleMs =
                IdleMilliseconds(sample.nowTick, sample.lastInputTick);
            ++summary.samples;
            switch (state) {
                case ActivityState::Locked:
                    ++summary.locked;
                    break;
                case ActivityState::Disconnected:
                    ++summary.disconnected;
                    break;
                case ActivityState::Idle:
                    ++summary.idle;
                    break;
                case ActivityState::Active:
                    ++summary.active;
                    break;
                default: // Unknown 不可能（input 成功时 ClassifyActivity 不产 Unknown）
                    ++summary.unknown;
                    break;
            }
            if (onSample) {
                onSample(state, idleMs);
            }
        }
        if (sampleInterval.count() > 0 && i + 1 < sampleCount) {
            std::this_thread::sleep_for(sampleInterval);
        }
    }
    return common::Result<ActivityContextSummary>::Success(summary);
}

std::shared_ptr<LastInputBackend> CreateWin32LastInputBackend() {
    return std::make_shared<Win32LastInputBackend>();
}

std::shared_ptr<SessionProbe> CreateWin32SessionProbe() {
    return std::make_shared<Win32SessionProbe>();
}

std::int64_t TickDeltaMs(std::uint32_t fromTick,
                         std::uint32_t toTick) noexcept {
    // 32 位回绕处理：无符号差在真实间隔 < 2^31 ms 时即正确的正间隔；符号位为负表示
    // toTick 早于 fromTick（回退/异源），按 0（不做负差）。
    const std::uint32_t raw = toTick - fromTick;
    const std::int32_t signedDelta = static_cast<std::int32_t>(raw);
    return signedDelta < 0 ? 0 : static_cast<std::int64_t>(signedDelta);
}

std::int64_t IdleMilliseconds(std::uint32_t nowTick,
                              std::uint32_t lastInputTick) noexcept {
    return TickDeltaMs(lastInputTick, nowTick);
}

ActivityState ClassifyActivity(std::uint32_t nowTick,
                               std::uint32_t lastInputTick,
                               std::int64_t idleThresholdMs) noexcept {
    const std::int64_t idleMs = IdleMilliseconds(nowTick, lastInputTick);
    // 负阈值退化恒 Active（调用方应保证阈值非负；此处防御）。
    if (idleThresholdMs < 0) {
        return ActivityState::Active;
    }
    return idleMs >= idleThresholdMs ? ActivityState::Idle
                                     : ActivityState::Active;
}

common::Result<ActivityWindowSummary> ObserveActivity(
    LastInputBackend& backend, std::size_t sampleCount,
    std::chrono::milliseconds sampleInterval, std::int64_t idleThresholdMs,
    const std::function<void(ActivityState, std::int64_t)>& onSample) {
    if (sampleCount == 0) {
        return common::Result<ActivityWindowSummary>::Failure(
            common::Error::Validation(
                "ObserveActivity", L"sampleCount 必须为正"));
    }
    ActivityWindowSummary summary;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        auto query = backend.Query();
        if (!query) {
            // 查询失败（非交互会话等）：按 Unknown 降级，不伪装成活跃/空闲。
            ++summary.samples;
            ++summary.unknown;
            if (onSample) {
                onSample(ActivityState::Unknown, 0);
            }
        } else {
            const auto& sample = query.Value();
            const ActivityState state = ClassifyActivity(
                sample.nowTick, sample.lastInputTick, idleThresholdMs);
            const std::int64_t idleMs =
                IdleMilliseconds(sample.nowTick, sample.lastInputTick);
            ++summary.samples;
            if (state == ActivityState::Idle) {
                ++summary.idle;
            } else {
                ++summary.active;
            }
            if (onSample) {
                onSample(state, idleMs);
            }
        }
        // 间隔为 0 时不等待（测试用）；前台有界由调用方窗口约束。
        if (sampleInterval.count() > 0 && i + 1 < sampleCount) {
            std::this_thread::sleep_for(sampleInterval);
        }
    }
    return common::Result<ActivityWindowSummary>::Success(summary);
}

} // namespace optimizer::activity
