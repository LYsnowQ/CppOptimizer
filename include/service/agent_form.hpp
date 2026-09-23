#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace optimizer::service {

// 常驻形态（形态决策：**默认的是“启动项 + 托盘”**；其余为高级可选形态）。
// 本枚举用于命令行显式选择形态做一次安装/移除动作；配置节 [agent].form 只表达**声明的意图**
// （受理默认形态与两个高级形态），**永不触发自动注册**。
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

// 声明式收敛的动作项：`install == true` 表示安装，`false` 表示卸载。
struct AgentFormAction {
    AgentForm form = AgentForm::StartupTray;
    bool install = true;
};

// 声明式收敛（半自动 `--agent-form apply`）：把实态收敛到 `declared`。
// 顺序保证“不会出现一个形态都没有”的窗口：**先安装声明形态（若未安装），再卸载其它已注册形态**。
// 已收敛时返回空列表（调用方据此报告“already converged”，不产生任何系统变更）。
[[nodiscard]] std::vector<AgentFormAction> ResolveFormApplyActions(
    AgentForm declared, bool startupInstalled, bool taskInstalled,
    bool serviceInstalled);

} // namespace optimizer::service
