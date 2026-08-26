#pragma once

#include "common/error.hpp"
#include "common/unique_resource.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace optimizer::service {

// 服务宿主运行模式。
enum class RunMode {
    Console,    // 控制台托管：前台、有界、Ctrl+C 优雅停止
    Service,    // Windows 服务模式：由 SCM 启动（StartServiceCtrlDispatcher）
    Install,    // 安装服务（需要管理员）
    Uninstall   // 卸载服务（需要管理员）
};

// 运行模式名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* RunModeToString(RunMode mode) noexcept;

// 解析运行模式（ASCII 大小写不敏感；未知文本返回 nullopt）。
[[nodiscard]] std::optional<RunMode> ParseRunMode(std::wstring_view text) noexcept;

// 服务元信息。
struct ServiceIdentity {
    std::wstring name;            // 服务名（SCM 唯一键；SCM 以 argv[1]==name 启动本进程）
    std::wstring displayName;     // 显示名（服务管理器中可见）
    std::wstring description;     // 描述（装饰性字段，写入失败不阻断安装）
    std::wstring executablePath;  // 安装时写入 ImagePath；为空时取当前模块路径
};

// 服务运行状态（与 SERVICE_STATUS.dwCurrentState 对应）。
enum class ServiceState {
    Unknown,      // 未开始上报
    StartPending, // SERVICE_START_PENDING
    Running,      // SERVICE_RUNNING
    StopPending,  // SERVICE_STOP_PENDING
    Stopped       // SERVICE_STOPPED（终态，服务进程随即退出）
};

// 状态名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* StateToString(ServiceState state) noexcept;

// 运行态接受的控制码：STOP | SHUTDOWN。
inline constexpr std::uint32_t kServiceControlsAccepted =
    SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

// 状态上报载荷（SERVICE_STATUS 字段的纯值版，便于不依赖 SCM 单测）。
struct ServiceStatusReport {
    ServiceState state = ServiceState::Unknown;
    std::uint32_t controlsAccepted = 0; // 仅 Running 携带 STOP|SHUTDOWN
    std::uint32_t win32ExitCode = 0;    // 非零表示启动/运行失败
    std::uint32_t checkPoint = 0;       // 长操作进度，从 1 起单调递增
    std::uint32_t waitHintMs = 0;       // 到达下一状态的预计最长毫秒数
};

// 构造指定状态的上报：Running 自动携带可接受控制码（STOP|SHUTDOWN），
// 其余状态 controlsAccepted 为 0（Pending/Stopped 期间不接受控制）。
[[nodiscard]] ServiceStatusReport MakeStatusReport(
    ServiceState state, std::uint32_t win32ExitCode = 0,
    std::uint32_t checkPoint = 0, std::uint32_t waitHintMs = 0) noexcept;

// 状态机转移合法性（纯函数）。合法序列：
// Unknown -> StartPending -> Running -> StopPending -> Stopped；
// 允许的旁路：StartPending -> StopPending/Stopped（启动期停止/失败）、
// Running -> Stopped（SCM 允许直接终态）。Stopped 为终态，其余转移返回 Validation。
[[nodiscard]] common::Result<void> ValidateStateTransition(
    ServiceState from, ServiceState to) noexcept;

// ---------- SCM 交互后端（可注入 fake 单测） ----------

// 控制回调：SCM 控制码 -> 动作。由 SCM 分发线程调用，不得抛异常、
// 不得调用阻塞 API；宿主内部转为停止请求与状态重报。
class ScmBackend {
public:
    virtual ~ScmBackend() = default;

    // 注册控制回调（RegisterServiceCtrlHandlerExW）。失败返回 Win32 错误。
    [[nodiscard]] virtual common::Result<void> RegisterControlHandler(
        std::wstring_view serviceName,
        std::function<void(std::uint32_t controlCode)> handler) = 0;

    // 上报状态（SetServiceStatus）。失败返回 Win32 错误。
    [[nodiscard]] virtual common::Result<void> ReportStatus(
        const ServiceStatusReport& report) = 0;

    // 注册服务主函数并进入 SCM 分发循环（StartServiceCtrlDispatcherW）。
    // 阻塞直到服务停止；当前进程非 SCM 启动时返回
    // ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 错误。
    [[nodiscard]] virtual common::Result<void> RunServiceDispatcher(
        std::wstring_view serviceName,
        std::function<common::Result<void>(std::wstring_view)> serviceMain) = 0;
};

// 真实 Win32 SCM 后端。
[[nodiscard]] std::shared_ptr<ScmBackend> CreateWin32ScmBackend();

// ---------- 服务安装/卸载（SCM 注册表操作，需要管理员） ----------

// 安装服务：OpenSCManager(SC_MANAGER_CREATE_SERVICE) + CreateServiceW
// （SERVICE_WIN32_OWN_PROCESS、SERVICE_DEMAND_START 保守默认：不自动启动）。
// executablePath 为空时取当前模块路径；ImagePath 带引号。
// 标准用户权限不足返回 ERROR_ACCESS_DENIED；已安装返回 ERROR_SERVICE_EXISTS。
[[nodiscard]] common::Result<void> InstallService(
    const ServiceIdentity& identity) noexcept;

// 卸载服务：OpenService(DELETE) + DeleteService。
// 服务正在运行时 SCM 标记删除，停止后移除；未安装返回 ERROR_SERVICE_DOES_NOT_EXIST。
[[nodiscard]] common::Result<void> UninstallService(
    std::wstring_view name) noexcept;

// ---------- 服务宿主 ----------

// 跨层基础设施：把调用方注入的业务负载托管为控制台（前台有界）或
// Windows 服务（SCM 生命周期）形态。服务模式不接受临时危险命令：
// 负载由调用方注入，危险动作仍须各自门禁（docs/23 第 3 节）。
// 实例单次运行：RunConsole/RunService 择一调用至多一次，停止请求粘性保持
// （停止后不可再运行；复用需新建实例）。
class ServiceHost {
public:
    // 业务负载：每个 tick 执行一次；失败时宿主停止循环并向上报告（失败不伪装成功）。
    using Workload = std::function<common::Result<void>()>;

    struct Options {
        ServiceIdentity identity;
        std::chrono::milliseconds tickInterval = std::chrono::milliseconds(1000);
    };

    explicit ServiceHost(Workload workload, Options options,
                         std::shared_ptr<ScmBackend> backend);
    ~ServiceHost() noexcept;

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    // 控制台模式：前台、有界运行 boundedFor；Ctrl+C/关闭信号触发优雅停止。
    // 成功表示正常停止；失败表示负载错误或信号处理器安装失败。
    [[nodiscard]] common::Result<void> RunConsole(
        std::chrono::seconds boundedFor) noexcept;

    // 服务模式：进入 SCM 分发循环并按状态机上报
    // START_PENDING -> RUNNING -> STOP_PENDING -> STOPPED。
    // 当前进程非 SCM 启动时返回 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT。
    [[nodiscard]] common::Result<void> RunService() noexcept;

    // 请求停止（幂等、非阻塞）。控制回调线程/控制台信号线程调用。
    void RequestStop() noexcept;

    [[nodiscard]] bool IsStopRequested() const noexcept;

private:
    // 控制码处理：STOP/SHUTDOWN -> RequestStop；INTERROGATE -> 重报当前状态；
    // 其余控制码忽略（未接受的控制不会到达回调，防御性处理）。
    void HandleControl(std::uint32_t controlCode) noexcept;
    // 主循环：执行负载直到停止请求或期限。
    [[nodiscard]] common::Result<void> RunLoop(
        std::optional<std::chrono::steady_clock::time_point> deadline) noexcept;
    // 服务主函数（SCM 分发线程执行）。
    [[nodiscard]] common::Result<void> RunServiceMain(
        std::wstring_view serviceName) noexcept;
    // 状态迁移 + 上报：先校验合法转移（黄色不变量），上报失败原样返回。
    [[nodiscard]] common::Result<void> Transit(
        ServiceState next, std::uint32_t win32ExitCode = 0,
        std::uint32_t checkPoint = 0, std::uint32_t waitHintMs = 0) noexcept;
    // 重报当前状态（INTERROGATE 应答）。
    [[nodiscard]] common::Result<void> ReReportCurrentStatus() noexcept;

    static constexpr std::uint32_t kWaitHintMs = 3000;

    Workload workload_;
    Options options_;
    std::shared_ptr<ScmBackend> backend_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<ServiceState> currentState_{ServiceState::Unknown};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace optimizer::service
