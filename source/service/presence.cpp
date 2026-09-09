#include "service/presence.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ctime>
#include <fstream>
#include <string>
#include <utility>

namespace optimizer::service {

const wchar_t* PresenceStateToString(PresenceState state) noexcept {
    switch (state) {
        case PresenceState::Unknown:
            return L"Unknown";
        case PresenceState::Present:
            return L"Present";
        case PresenceState::Away:
            return L"Away";
    }
    return L"Unknown";
}

PresenceState ClassifyPresence(
    const std::optional<std::uint32_t>& idleSeconds,
    std::uint32_t awayAfterSeconds) noexcept {
    if (!idleSeconds) {
        return PresenceState::Unknown; // 未上报空闲：不伪装在场
    }
    if (awayAfterSeconds == 0) {
        return PresenceState::Present; // 不启用“不在场”判定：已知空闲即在场
    }
    return *idleSeconds < awayAfterSeconds ? PresenceState::Present
                                           : PresenceState::Away;
}

std::uint32_t EffectivePresenceAwaySeconds(
    std::uint32_t policyAwayIdleSeconds,
    std::uint32_t fallback) noexcept {
    return policyAwayIdleSeconds > 0 ? policyAwayIdleSeconds : fallback;
}

namespace {

// 本地时间戳（ASCII "YYYY-MM-DD HH:MM:SS"）。localtime_s 失败返回空串（行仍可写）。
std::string LocalTimestampAscii() noexcept {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local{};
    if (::localtime_s(&local, &now) != 0) {
        return "";
    }
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) ==
        0) {
        return "";
    }
    return std::string(buffer);
}

} // namespace

common::Result<void> AppendPresenceTransitionLine(
    const std::filesystem::path& path, PresenceState from,
    PresenceState to) noexcept {
    if (path.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "AppendPresenceTransitionLine", L"路径不能为空"));
    }
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(ec.value()),
                "create_directories(presence timeline)"));
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open presence timeline for append"));
    }
    // 状态名为 ASCII：逐宽字符收窄直写（与文件 UTF-8 兼容，避免窄化告警）。
    const auto narrowAscii = [](const wchar_t* text) {
        std::string out;
        for (const wchar_t* p = text; p != nullptr && *p != L'\0'; ++p) {
            out.push_back(static_cast<char>(*p));
        }
        return out;
    };
    out << LocalTimestampAscii() << " "
        << narrowAscii(PresenceStateToString(from)) << " -> "
        << narrowAscii(PresenceStateToString(to)) << "\n";
    out.flush();
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "flush presence timeline"));
    }
    return common::Result<void>::Success();
}

HostPresenceTracker::HostPresenceTracker(Options options)
    : options_(std::move(options)) {}

void HostPresenceTracker::PruneExpired() noexcept {
    if (options_.forgetAfter <= std::chrono::seconds::zero()) {
        return;
    }
    const auto cutoff = options_.now() - options_.forgetAfter;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.lastSeen < cutoff) {
            it = entries_.erase(it); // 客户端停止上报超时：从台账移除
        } else {
            ++it;
        }
    }
}

PresenceState HostPresenceTracker::Record(
    std::string_view clientKey,
    const std::optional<std::uint32_t>& idleSeconds) noexcept {
    if (clientKey.empty()) {
        return PresenceState::Unknown; // 无效键：不记录
    }
    PruneExpired();
    const auto state = ClassifyPresence(idleSeconds, options_.awayAfterSeconds);
    auto& entry = entries_[std::string(clientKey)];
    entry.state = state;
    entry.idleSeconds = idleSeconds;
    entry.lastSeen = options_.now();
    return state;
}

void HostPresenceTracker::SetOnChange(ChangeCallback callback) noexcept {
    onChange_ = std::move(callback);
}

PresenceState HostPresenceTracker::ComputeSummary() const noexcept {
    bool anyKnown = false;
    bool anyPresent = false;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        if (entry.state != PresenceState::Unknown) {
            anyKnown = true;
            if (entry.state == PresenceState::Present) {
                anyPresent = true;
            }
        }
    }
    if (!anyKnown) {
        return PresenceState::Unknown;
    }
    // “有人在”语义：任一在场即 Present，否则全部 Away -> Away。
    return anyPresent ? PresenceState::Present : PresenceState::Away;
}

PresenceState HostPresenceTracker::Summary() noexcept {
    PruneExpired();
    const PresenceState current = ComputeSummary();
    if (current != lastSummary_) {
        const PresenceState previous = lastSummary_;
        lastSummary_ = current;
        if (onChange_) {
            onChange_(previous, current); // 变化事件：在场度时间线（仅观测）
        }
    }
    return current;
}

std::vector<ClientPresence> HostPresenceTracker::Clients() noexcept {
    PruneExpired();
    std::vector<ClientPresence> clients;
    clients.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        ClientPresence item;
        item.key = key;
        item.state = entry.state;
        item.idleSeconds = entry.idleSeconds;
        clients.push_back(std::move(item));
    }
    return clients;
}

std::size_t HostPresenceTracker::ClientCount() noexcept {
    PruneExpired();
    return entries_.size();
}

} // namespace optimizer::service
