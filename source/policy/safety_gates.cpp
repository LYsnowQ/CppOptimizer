#include "policy/safety_gates.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <utility>

namespace optimizer::policy {

const char* GateIdToString(GateId gate) noexcept {
    switch (gate) {
        case GateId::CompileTime:
            return "compile";
        case GateId::Config:
            return "config";
        case GateId::CommandLine:
            return "cmdline";
        case GateId::PermissionAndEnvironment:
            return "perm_env";
        case GateId::Audit:
            return "audit";
        case GateId::Cooldown:
            return "cooldown";
    }
    return "unknown";
}

GateEvaluation EvaluateGates(const GateInputs& inputs) noexcept {
    GateEvaluation evaluation;
    evaluation.gates = inputs;
    // 逐门按固定顺序判定：第一个未通过的门即“先卡在哪一道”。
    const struct {
        GateId id;
        bool passed;
    } ordered[] = {
        {GateId::CompileTime, inputs.compileTime},
        {GateId::Config, inputs.config},
        {GateId::CommandLine, inputs.commandLine},
        {GateId::PermissionAndEnvironment, inputs.permissionAndEnvironment},
        {GateId::Audit, inputs.audit},
        {GateId::Cooldown, inputs.cooldown},
    };
    evaluation.allowed = true;
    for (const auto& gate : ordered) {
        if (!gate.passed) {
            evaluation.allowed = false;
            if (!evaluation.firstBlocking.has_value()) {
                evaluation.firstBlocking = gate.id;
            }
        }
    }
    return evaluation;
}

bool EvaluateEnvironmentGate(const EnvironmentFacts& facts) noexcept {
    // 任一不安全或未知情形即不通过：门禁宁可误拒，不可误放。
    return facts.factsKnown && facts.osSupported && !facts.onBattery &&
           !facts.remoteSession && !facts.sessionLocked &&
           facts.interactiveSession;
}

bool EvaluateCooldownGate(const CooldownLedger& ledger,
                          std::string_view capabilityId,
                          std::int64_t nowUnixSeconds,
                          std::chrono::seconds cooldown) noexcept {
    const auto found = ledger.lastRunUnixSeconds.find(capabilityId);
    if (found == ledger.lastRunUnixSeconds.end() || found->second <= 0) {
        return true; // 无记录：从未执行过，冷却门不拦
    }
    const std::int64_t elapsed = nowUnixSeconds - found->second;
    if (elapsed < 0) {
        return false; // 时钟回拨：按“仍在窗口内”保守处理
    }
    return elapsed >= static_cast<std::int64_t>(cooldown.count());
}

common::Result<CooldownLedger> ReadCooldownLedger(
    const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return common::Result<CooldownLedger>::Failure(common::Error::Validation(
            "ReadCooldownLedger", L"路径不能为空"));
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return common::Result<CooldownLedger>::Failure(common::Error::Validation(
            "ReadCooldownLedger", L"冷却台账路径是目录，不是文件"));
    }
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            return common::Result<CooldownLedger>::Failure(
                common::Error::FromWin32(static_cast<std::uint32_t>(ec.value()),
                                         "exists(cooldown ledger)"));
        }
        return common::Result<CooldownLedger>::Success(CooldownLedger{}); // 尚无记录
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return common::Result<CooldownLedger>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open cooldown ledger"));
    }
    CooldownLedger ledger;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            if (line != kCooldownEnvelope) {
                return common::Result<CooldownLedger>::Success(
                    CooldownLedger{}); // 信封不符：不当作台账（宁可全部重新计时）
            }
            continue;
        }
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string::npos) {
            continue; // 单行畸形：跳过（不阻断整份台账）
        }
        try {
            ledger.lastRunUnixSeconds[line.substr(0, space)] =
                std::stoll(line.substr(space + 1));
        } catch (...) {
            continue; // 无法解析的数值：跳过
        }
    }
    return common::Result<CooldownLedger>::Success(std::move(ledger));
}

common::Result<void> WriteCooldownLedger(
    const std::filesystem::path& path, const CooldownLedger& ledger) noexcept {
    if (path.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "WriteCooldownLedger", L"路径不能为空"));
    }
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(ec.value()),
                "create_directories(cooldown ledger)"));
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open cooldown ledger for write"));
    }
    out << kCooldownEnvelope << '\n';
    for (const auto& entry : ledger.lastRunUnixSeconds) {
        out << entry.first << ' ' << entry.second << '\n';
    }
    out.flush();
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "flush cooldown ledger"));
    }
    return common::Result<void>::Success();
}

} // namespace optimizer::policy
