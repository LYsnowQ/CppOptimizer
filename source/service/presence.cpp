#include "service/presence.hpp"

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
