#include "activity/user_activity.hpp"

#include <windows.h>

#include <thread>

namespace optimizer::activity {

namespace {

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
    }
    return L"unknown";
}

std::shared_ptr<LastInputBackend> CreateWin32LastInputBackend() {
    return std::make_shared<Win32LastInputBackend>();
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
