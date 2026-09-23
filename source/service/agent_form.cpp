#include "service/agent_form.hpp"

#include <string>

namespace optimizer::service {

namespace {

// 归一化：ASCII 小写 + 横线下划线互换（命令行书写习惯差异不改变语义）。
std::string NormalizeAscii(std::wstring_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch > 0x7F) {
            return std::string(); // 非 ASCII：不可能匹配任何形态名（不做宽松猜测）
        }
        char narrow = static_cast<char>(ch);
        if (narrow == '-') {
            narrow = '_';
        }
        if (narrow >= 'A' && narrow <= 'Z') {
            narrow = static_cast<char>(narrow - 'A' + 'a');
        }
        normalized.push_back(narrow);
    }
    return normalized;
}

} // namespace

const wchar_t* AgentFormToString(AgentForm form) noexcept {
    switch (form) {
        case AgentForm::StartupTray:
            return L"startup_tray";
        case AgentForm::ScheduledTask:
            return L"task";
        case AgentForm::Service:
            return L"service";
    }
    return L"unknown";
}

std::optional<AgentForm> ParseAgentForm(std::wstring_view text) noexcept {
    const std::string normalized = NormalizeAscii(text);
    if (normalized == "startup_tray") {
        return AgentForm::StartupTray;
    }
    if (normalized == "task") {
        return AgentForm::ScheduledTask;
    }
    if (normalized == "service") {
        return AgentForm::Service;
    }
    return std::nullopt; // 未知形态：拒绝，不猜
}

bool AgentFormRequiresElevation(AgentForm form) noexcept {
    // 启动项写 HKCU，标准用户即可；计划任务与服务属注册类提权动作。
    return form != AgentForm::StartupTray;
}

bool IsDefaultAgentForm(AgentForm form) noexcept {
    return form == DefaultAgentForm();
}

std::vector<AgentForm> ConflictingAgentForms(AgentForm target,
                                             bool startupInstalled,
                                             bool taskInstalled,
                                             bool serviceInstalled) {
    // 形态固定顺序（启动项 -> 计划任务 -> 服务）：输出稳定，便于展示与测试。
    std::vector<AgentForm> conflicts;
    if (target != AgentForm::StartupTray && startupInstalled) {
        conflicts.push_back(AgentForm::StartupTray);
    }
    if (target != AgentForm::ScheduledTask && taskInstalled) {
        conflicts.push_back(AgentForm::ScheduledTask);
    }
    if (target != AgentForm::Service && serviceInstalled) {
        conflicts.push_back(AgentForm::Service);
    }
    return conflicts;
}

} // namespace optimizer::service
