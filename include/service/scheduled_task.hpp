#pragma once

#include "common/error.hpp"

#include <memory>
#include <string>

namespace optimizer::service {

// 计划任务（每用户，Task Scheduler 2.0）：以“登录后拉起常驻宿主”的方式注册同一个命令行。
// 与自启动项的差别：计划任务额外提供“错过触发后补跑”等调度语义，代价是需要管理员权限注册。
//
// 契约（权限分界，与危险操作策略一致）：
// - **注册类一次性显式动作**：只有显式调用 Install 才创建任务；运行期不提权、不在后台静默创建；
// - **不提权运行**：任务以**当前用户交互令牌**（TASK_LOGON_INTERACTIVE_TOKEN）运行，进程不被提升；
//   `OnBoot` 触发需要以 SYSTEM 运行，与本项目“托盘与 Agent 进程永不提权”的分界冲突，
//   因此 Install 直接以 Validation 拒绝（显式拒绝而非静默提权）；
// - **需要管理员**：非提升进程注册会被系统拒绝，此时如实返回错误域（不伪装成功、不降级跳过）；
// - **可逆**：Remove 删除同名任务（不存在视为成功 = 幂等），卸载后不得残留任务；
// - **查询不是错误**：未注册返回 `installed == false`（Success）。
inline constexpr const wchar_t* kScheduledTaskName = L"CppOptimizer";

// 触发方式。仅 `OnLogon` 可安装；`OnBoot` 保留为受控取值，用于如实拒绝（见上）。
enum class TaskTrigger {
    OnLogon,
    OnBoot,
};

// 计划任务规格：任务名 + 完整命令行 + 触发方式。
struct ScheduledTaskSpec {
    std::wstring taskName = kScheduledTaskName;
    std::wstring commandLine;
    TaskTrigger trigger = TaskTrigger::OnLogon;
};

// 查询结果：`installed == false` 表示未注册（Success，不是错误）。
struct ScheduledTaskStatus {
    bool installed = false;
    std::wstring commandLine; // 已注册任务的执行命令行（读取失败时为空）
    std::wstring trigger;     // "logon" / "boot"；无法判定时为空
};

// 可注入后端：把“查/建/删计划任务”抽成接口，使单测用 fake 而不触碰真实任务库。
// 契约：Read 未注册返回 installed=false（不是错误）；Create 收到完整命令行与触发方式；
// Remove 删除不存在视为成功（幂等）；任何失败如实返回错误域，不得伪成功。
class ScheduledTaskBackend {
public:
    virtual ~ScheduledTaskBackend() = default;

    [[nodiscard]] virtual common::Result<ScheduledTaskStatus> Read(
        const std::wstring& taskName) = 0;
    [[nodiscard]] virtual common::Result<void> Create(
        const ScheduledTaskSpec& spec) = 0;
    [[nodiscard]] virtual common::Result<void> Remove(
        const std::wstring& taskName) = 0;
};

// 面向后端的操作（纯逻辑：空任务名/空命令行/受控触发方式校验；校验失败不调用后端）。
[[nodiscard]] common::Result<void> InstallScheduledTask(
    ScheduledTaskBackend& backend, const ScheduledTaskSpec& spec) noexcept;
[[nodiscard]] common::Result<void> RemoveScheduledTask(
    ScheduledTaskBackend& backend, const std::wstring& taskName) noexcept;
[[nodiscard]] common::Result<ScheduledTaskStatus> QueryScheduledTask(
    ScheduledTaskBackend& backend, const std::wstring& taskName) noexcept;

// COM 实现（taskschd.dll / ITaskService）；分配失败返回 nullptr。
[[nodiscard]] std::shared_ptr<ScheduledTaskBackend>
CreateWin32ScheduledTaskBackend() noexcept;

// 免费函数重载（内部创建 COM 后端）：供命令行直接调用。
[[nodiscard]] common::Result<void> InstallScheduledTask(
    const ScheduledTaskSpec& spec) noexcept;
[[nodiscard]] common::Result<void> RemoveScheduledTask(
    const std::wstring& taskName) noexcept;
[[nodiscard]] common::Result<ScheduledTaskStatus> QueryScheduledTask(
    const std::wstring& taskName) noexcept;

} // namespace optimizer::service
