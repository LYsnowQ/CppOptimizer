#include "policy/safety_gates.hpp"

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

} // namespace optimizer::policy
