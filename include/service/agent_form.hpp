#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace optimizer::service {

// 常驻形态（形态决策：**默认且不可更改**的是“启动项 + 托盘”；其余为高级可选形态）。
// 本枚举用于命令行显式选择形态做一次安装/移除动作；**不代表配置取值已开放**——
// 配置节 [agent].form 当前仍只接受默认形态，取值开放属后续切片。
enum class AgentForm {
    StartupTray,    // 启动项 + 托盘（默认；标准用户、无提权）
    ScheduledTask,  // 计划任务（登录触发；注册需管理员，任务本身不提权）
    Service,        // SCM 服务（安装需管理员；Session 0，无托盘界面）
};

// 默认形态（唯一默认且不可更改）。
[[nodiscard]] constexpr AgentForm DefaultAgentForm() noexcept {
    return AgentForm::StartupTray;
}

// 形态名（ASCII，恒成功）："startup_tray" / "task" / "service"。
[[nodiscard]] const wchar_t* AgentFormToString(AgentForm form) noexcept;

// 解析形态名（ASCII 大小写不敏感，接受横线下划线互换的书写：startup-tray 等）；
// 未知文本返回 nullopt（不做宽松猜测）。
[[nodiscard]] std::optional<AgentForm> ParseAgentForm(
    std::wstring_view text) noexcept;

// 注册该形态是否需要管理员一次性提权（运行身份与注册权限是两件事：
// 计划任务与服务虽需管理员注册，但运行身份不得被提升）。
[[nodiscard]] bool AgentFormRequiresElevation(AgentForm form) noexcept;

// 是否为唯一默认形态（用于“默认不可更改”的显式校验）。
[[nodiscard]] bool IsDefaultAgentForm(AgentForm form) noexcept;

// 形态互斥（单一形态生效）：安装 `target` 时，已注册的**其它**形态即为冲突。
// 存在冲突时不自动卸载任何形态——由调用方要求用户显式选择（替换或先卸载）。
[[nodiscard]] std::vector<AgentForm> ConflictingAgentForms(AgentForm target,
                                                           bool startupInstalled,
                                                           bool taskInstalled,
                                                           bool serviceInstalled);

} // namespace optimizer::service
