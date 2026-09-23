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
    // ”临时文件 + 原子替换“：任何一步失败都不破坏已有台账（一个写坏的台账会让冷却门
    // 对全部能力放行或永久拦住，不能拿它冒险）。
    std::filesystem::path tempPath = path;
    tempPath += L".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(::GetLastError()),
                "open cooldown ledger temp file for write"));
        }
        out << kCooldownEnvelope << '\n';
        for (const auto& entry : ledger.lastRunUnixSeconds) {
            out << entry.first << ' ' << entry.second << '\n';
        }
        out.flush();
        if (!out) {
            out.close();
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(::GetLastError()),
                "flush cooldown ledger temp file"));
        }
        out.close();
    }
    if (!::MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = static_cast<std::uint32_t>(::GetLastError());
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc); // 失败不留下半成品临时文件
        return common::Result<void>::Failure(
            common::Error::FromWin32(code, "MoveFileExW(cooldown ledger)"));
    }
    return common::Result<void>::Success();
}

CooldownLedger WithCooldownRun(const CooldownLedger& ledger,
                               std::string_view capabilityId,
                               std::int64_t nowUnixSeconds) noexcept {
    CooldownLedger updated = ledger;
    if (capabilityId.empty() || nowUnixSeconds <= 0) {
        return updated; // 无效输入：台账不变（不写入误导性时刻）
    }
    const auto found = updated.lastRunUnixSeconds.find(capabilityId);
    if (found != updated.lastRunUnixSeconds.end() &&
        found->second > nowUnixSeconds) {
        return updated; // 时钟回拨：保留更晚的记录（不得缩短冷却窗口）
    }
    updated.lastRunUnixSeconds[std::string(capabilityId)] = nowUnixSeconds;
    return updated;
}

common::Result<void> RecordCooldownRun(const std::filesystem::path& path,
                                       std::string_view capabilityId,
                                       std::int64_t nowUnixSeconds) noexcept {
    if (path.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RecordCooldownRun", L"路径不能为空"));
    }
    if (capabilityId.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RecordCooldownRun", L"能力 ID 不能为空"));
    }
    const auto ledger = ReadCooldownLedger(path);
    if (!ledger) {
        // 读失败不得当作“空台账”覆盖：否则一次读故障会清掉全部冷却记录。
        return common::Result<void>::Failure(ledger.ErrorValue());
    }
    return WriteCooldownLedger(
        path, WithCooldownRun(ledger.Value(), capabilityId, nowUnixSeconds));
}

std::string FormatGatesJson(const GatesReport& report) {
    std::string json = "{\"readOnly\":";
    json += report.readOnly ? "true" : "false";
    json += ",\"acknowledged\":";
    json += report.acknowledged ? "true" : "false";
    json += ",\"environment\":{\"factsKnown\":";
    json += report.factsKnown ? "true" : "false";
    json += ",\"osSupported\":";
    json += report.osSupported ? "true" : "false";
    json += ",\"onBattery\":";
    json += report.onBattery ? "true" : "false";
    json += ",\"remoteSession\":";
    json += report.remoteSession ? "true" : "false";
    json += ",\"sessionLocked\":";
    json += report.sessionLocked ? "true" : "false";
    json += ",\"auditWritable\":";
    json += report.auditWritable ? "true" : "false";
    json += "},\"capabilities\":[";
    for (std::size_t i = 0; i < report.capabilities.size(); ++i) {
        const auto& entry = report.capabilities[i];
        if (i > 0) {
            json += ",";
        }
        json += "{\"name\":\"";
        json += entry.name;
        json += "\",\"allowed\":";
        json += entry.allowed ? "true" : "false";
        json += ",\"firstBlocking\":";
        if (entry.firstBlocking.empty()) {
            json += "null";
        } else {
            json += "\"";
            json += entry.firstBlocking;
            json += "\"";
        }
        json += "}";
    }
    json += "]}";
    return json;
}

} // namespace optimizer::policy
