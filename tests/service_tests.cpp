#include "service/service_host.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::ErrorDomain;
using optimizer::common::Result;
using optimizer::service::CreateWin32ScmBackend;
using optimizer::service::InstallService;
using optimizer::service::MakeStatusReport;
using optimizer::service::ParseRunMode;
using optimizer::service::RunMode;
using optimizer::service::RunModeToString;
using optimizer::service::ScmBackend;
using optimizer::service::ServiceHost;
using optimizer::service::ServiceIdentity;
using optimizer::service::ServiceState;
using optimizer::service::ServiceStatusReport;
using optimizer::service::StateToString;
using optimizer::service::UninstallService;
using optimizer::service::ValidateStateTransition;

// ---------- SCM fake（记录上报序列，可注入控制码与失败） ----------

class FakeScmBackend final : public ScmBackend {
public:
    bool failRegister = false;
    bool failReport = false;
    bool failDispatcher = false;
    std::uint32_t dispatcherError = ERROR_SUCCESS;

    std::vector<ServiceState> reportedStates;
    std::vector<ServiceStatusReport> reports;
    std::vector<std::wstring> dispatcherNames;
    std::function<void(std::uint32_t)> controlHandler;

    Result<void> RegisterControlHandler(
        std::wstring_view serviceName,
        std::function<void(std::uint32_t)> handler) override {
        (void)serviceName;
        if (failRegister) {
            return Result<void>::Failure(Error::FromWin32(
                ERROR_ACCESS_DENIED, "FakeScmBackend::RegisterControlHandler"));
        }
        controlHandler = std::move(handler);
        return Result<void>::Success();
    }

    Result<void> ReportStatus(const ServiceStatusReport& report) override {
        if (failReport) {
            return Result<void>::Failure(Error::FromWin32(
                ERROR_ACCESS_DENIED, "FakeScmBackend::ReportStatus"));
        }
        reports.push_back(report);
        reportedStates.push_back(report.state);
        return Result<void>::Success();
    }

    Result<void> RunServiceDispatcher(
        std::wstring_view serviceName,
        std::function<Result<void>(std::wstring_view)> serviceMain) override {
        if (failDispatcher) {
            return Result<void>::Failure(
                Error::FromWin32(dispatcherError, "FakeScmBackend::RunServiceDispatcher"));
        }
        dispatcherNames.emplace_back(serviceName);
        // 同步模拟 SCM 启动服务主函数（与真实分发阻塞语义一致）。
        return serviceMain(serviceName);
    }

    // 模拟 SCM 向控制回调投递控制码。
    void SimulateControl(std::uint32_t controlCode) {
        if (controlHandler) {
            controlHandler(controlCode);
        }
    }
};

ServiceHost::Options MakeOptions() {
    ServiceHost::Options options;
    options.identity.name = L"CppOptimizerService";
    options.identity.displayName = L"CppOptimizer Service";
    options.tickInterval = std::chrono::milliseconds(10);
    return options;
}

// ---------- 运行模式解析 ----------

bool TestParseRunMode() {
    return ParseRunMode(L"console") == RunMode::Console &&
           ParseRunMode(L"CONSOLE") == RunMode::Console &&
           ParseRunMode(L"service") == RunMode::Service &&
           ParseRunMode(L"install") == RunMode::Install &&
           ParseRunMode(L"Uninstall") == RunMode::Uninstall &&
           !ParseRunMode(L"") && !ParseRunMode(L"wat") &&
           !ParseRunMode(L"consolex");
}

bool TestRunModeToStringRoundTrip() {
    for (const auto mode : {RunMode::Console, RunMode::Service,
                            RunMode::Install, RunMode::Uninstall}) {
        const wchar_t* name = RunModeToString(mode);
        if (name == nullptr || ParseRunMode(name) != mode) {
            return false;
        }
    }
    return true;
}

// ---------- 服务状态机 ----------

bool TestStateMachineLegalPath() {
    // 规范生命周期全合法。
    return ValidateStateTransition(ServiceState::Unknown,
                                   ServiceState::StartPending) &&
           ValidateStateTransition(ServiceState::StartPending,
                                   ServiceState::Running) &&
           ValidateStateTransition(ServiceState::Running,
                                   ServiceState::StopPending) &&
           ValidateStateTransition(ServiceState::StopPending,
                                   ServiceState::Stopped);
}

bool TestStateMachineStartupAbortPaths() {
    // 启动期停止/失败旁路与直接终态合法。
    return ValidateStateTransition(ServiceState::StartPending,
                                   ServiceState::StopPending) &&
           ValidateStateTransition(ServiceState::StartPending,
                                   ServiceState::Stopped) &&
           ValidateStateTransition(ServiceState::Running,
                                   ServiceState::Stopped);
}

bool TestStateMachineIllegalTransitions() {
    // 回退、自循环、终态复活的转移全部非法。
    const auto bad = [](ServiceState from, ServiceState to) {
        return !ValidateStateTransition(from, to);
    };
    return bad(ServiceState::Running, ServiceState::StartPending) &&
           bad(ServiceState::Running, ServiceState::Running) &&
           bad(ServiceState::Stopped, ServiceState::Running) &&
           bad(ServiceState::Stopped, ServiceState::StartPending) &&
           bad(ServiceState::StopPending, ServiceState::Running) &&
           bad(ServiceState::Unknown, ServiceState::Running) &&
           bad(ServiceState::Unknown, ServiceState::Unknown);
}

bool TestMakeStatusReportRunningAcceptsControls() {
    const auto report = MakeStatusReport(ServiceState::Running);
    return report.state == ServiceState::Running &&
           (report.controlsAccepted & SERVICE_ACCEPT_STOP) != 0 &&
           (report.controlsAccepted & SERVICE_ACCEPT_SHUTDOWN) != 0;
}

bool TestMakeStatusReportPendingAcceptsNothing() {
    const auto start = MakeStatusReport(ServiceState::StartPending, 0, 1, 3000);
    const auto stopped = MakeStatusReport(ServiceState::Stopped, 7);
    return start.controlsAccepted == 0 && start.checkPoint == 1 &&
           start.waitHintMs == 3000 && start.win32ExitCode == 0 &&
           stopped.controlsAccepted == 0 && stopped.win32ExitCode == 7 &&
           stopped.state == ServiceState::Stopped;
}

bool TestStateToString() {
    return std::wstring(StateToString(ServiceState::Running)) == L"running" &&
           std::wstring(StateToString(ServiceState::Stopped)) == L"stopped";
}

// ---------- 服务主流程（fake 分发） ----------

bool TestServiceFlowStatusSequence() {
    auto fake = std::make_shared<FakeScmBackend>();
    ServiceHost* hostPtr = nullptr;
    std::size_t ticks = 0;
    ServiceHost host(
        [&] {
            ++ticks;
            if (hostPtr != nullptr) {
                hostPtr->RequestStop(); // 首个 tick 后请求停止（确定性停止）
            }
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    hostPtr = &host;

    const auto result = host.RunService();
    if (!result) {
        return false;
    }
    // 状态序列必须为 START_PENDING -> RUNNING -> STOP_PENDING -> STOPPED。
    const std::vector<ServiceState> expected = {
        ServiceState::StartPending, ServiceState::Running,
        ServiceState::StopPending, ServiceState::Stopped};
    if (fake->reportedStates != expected) {
        return false;
    }
    if (fake->dispatcherNames.size() != 1 ||
        fake->dispatcherNames[0] != L"CppOptimizerService") {
        return false;
    }
    // 首个 tick 内请求停止：只执行了一次负载。
    return ticks == 1;
}

bool TestControlStopRequestsStop() {
    auto fake = std::make_shared<FakeScmBackend>();
    bool stoppedViaControl = false;
    ServiceHost host(
        [&] {
            // 首个 tick：模拟用户 sc stop 投递 STOP 控制码。
            if (!stoppedViaControl) {
                stoppedViaControl = true;
                fake->SimulateControl(SERVICE_CONTROL_STOP);
            }
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    const auto result = host.RunService();
    if (!result || !stoppedViaControl) {
        return false;
    }
    const std::vector<ServiceState> expected = {
        ServiceState::StartPending, ServiceState::Running,
        ServiceState::StopPending, ServiceState::Stopped};
    return fake->reportedStates == expected;
}

bool TestControlInterrogateRepeatsCurrentStatus() {
    auto fake = std::make_shared<FakeScmBackend>();
    bool interrogated = false;
    ServiceHost host(
        [&] {
            if (!interrogated) {
                interrogated = true;
                // INTERROGATE 应答重报当前状态（Running），随后 STOP。
                fake->SimulateControl(SERVICE_CONTROL_INTERROGATE);
                fake->SimulateControl(SERVICE_CONTROL_STOP);
            }
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    const auto result = host.RunService();
    if (!result) {
        return false;
    }
    const std::vector<ServiceState> expected = {
        ServiceState::StartPending, ServiceState::Running,
        ServiceState::Running, // INTERROGATE 重报
        ServiceState::StopPending, ServiceState::Stopped};
    return fake->reportedStates == expected;
}

bool TestControlUnknownCodeIgnored() {
    auto fake = std::make_shared<FakeScmBackend>();
    bool controlled = false;
    ServiceHost host(
        [&] {
            if (!controlled) {
                controlled = true;
                // 未接受的控制码（PAUSE）必须被忽略，不触发停止。
                fake->SimulateControl(SERVICE_CONTROL_PAUSE);
                fake->SimulateControl(SERVICE_CONTROL_STOP);
            }
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    const auto result = host.RunService();
    if (!result) {
        return false;
    }
    const std::vector<ServiceState> expected = {
        ServiceState::StartPending, ServiceState::Running,
        ServiceState::StopPending, ServiceState::Stopped};
    return fake->reportedStates == expected;
}

bool TestControlShutdownRequestsStop() {
    // SERVICE_CONTROL_SHUTDOWN（系统关机）同样触发优雅停止。
    auto fake = std::make_shared<FakeScmBackend>();
    ServiceHost host(
        [&] {
            fake->SimulateControl(SERVICE_CONTROL_SHUTDOWN);
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    const auto result = host.RunService();
    return result && fake->reportedStates.size() == 4;
}

bool TestDispatcherConnectFailureNotDisguised() {
    // 非 SCM 启动（普通命令行）-> ERROR_FAILED_SERVICE_CONTROLLER_CONNECT。
    auto fake = std::make_shared<FakeScmBackend>();
    fake->failDispatcher = true;
    fake->dispatcherError = ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
    ServiceHost host([] { return Result<void>::Success(); }, MakeOptions(), fake);
    const auto result = host.RunService();
    return !result && result.ErrorValue().code ==
                         ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
}

bool TestRegisterControlHandlerFailureReported() {
    auto fake = std::make_shared<FakeScmBackend>();
    fake->failRegister = true;
    ServiceHost host([] { return Result<void>::Success(); }, MakeOptions(), fake);
    const auto result = host.RunService();
    // 注册失败：不进入任何状态上报。
    return !result && fake->reportedStates.empty();
}

bool TestWorkloadFailureStopsWithError() {
    // 负载失败：状态机走到 STOPPED（退出码非零），错误原样向上。
    auto fake = std::make_shared<FakeScmBackend>();
    ServiceHost host(
        [] {
            return Result<void>::Failure(
                Error::Validation("workload", L"injected tick failure"));
        },
        MakeOptions(), fake);
    const auto result = host.RunService();
    if (result) {
        return false;
    }
    if (fake->reportedStates.size() != 4) {
        return false;
    }
    const auto& stopped = fake->reports.back();
    return stopped.state == ServiceState::Stopped &&
           stopped.win32ExitCode != 0 &&
           result.ErrorValue().operation == "workload";
}

bool TestReportStatusFailureReported() {
    // 状态上报失败：不伪装成功。
    auto fake = std::make_shared<FakeScmBackend>();
    fake->failReport = true;
    ServiceHost host([] { return Result<void>::Success(); }, MakeOptions(), fake);
    const auto result = host.RunService();
    return !result;
}

// ---------- 控制台模式 ----------

bool TestConsoleBoundedRunsWorkload() {
    auto fake = std::make_shared<FakeScmBackend>();
    std::size_t ticks = 0;
    ServiceHost host(
        [&] {
            ++ticks;
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    const auto result = host.RunConsole(std::chrono::seconds(1));
    // 有界 1 秒 + tick 间隔 10ms：至少执行 1 个 tick。
    return result && ticks >= 1;
}

bool TestRequestStopEndsConsoleEarly() {
    auto fake = std::make_shared<FakeScmBackend>();
    std::size_t ticks = 0;
    ServiceHost host(
        [&] {
            ++ticks;
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    host.RequestStop(); // 停止请求粘性保持：循环首轮即退出，不执行任何 tick。
    const auto result = host.RunConsole(std::chrono::seconds(1));
    return result && ticks == 0 && host.IsStopRequested();
}

bool TestRequestStopIdempotent() {
    auto fake = std::make_shared<FakeScmBackend>();
    ServiceHost host([] { return Result<void>::Success(); }, MakeOptions(), fake);
    host.RequestStop();
    host.RequestStop(); // 重复请求无异常
    return host.IsStopRequested();
}

bool TestConsoleZeroDurationRejected() {
    auto fake = std::make_shared<FakeScmBackend>();
    ServiceHost host([] { return Result<void>::Success(); }, MakeOptions(), fake);
    const auto result = host.RunConsole(std::chrono::seconds(0));
    return !result &&
           result.ErrorValue().domain == ErrorDomain::Validation;
}

// ---------- 安装/卸载参数校验（不触碰真实 SCM） ----------

bool TestInstallRejectsEmptyNames() {
    const ServiceIdentity identity; // name/displayName 为空
    const auto result = InstallService(identity);
    return !result &&
           result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestUninstallRejectsEmptyName() {
    const auto result = UninstallService(L"");
    return !result &&
           result.ErrorValue().domain == ErrorDomain::Validation;
}

// ---------- 真实后端（非 SCM 启动路径，无需管理员） ----------

bool TestWin32BackendRejectsNonScmDispatcher() {
    // 真实后端在非 SCM 启动的进程中必须失败并报告连接错误。
    auto backend = CreateWin32ScmBackend();
    const auto result = backend->RunServiceDispatcher(
        L"CppOptimizerService",
        [](std::wstring_view) { return Result<void>::Success(); });
    return !result &&
           result.ErrorValue().code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
}

// 手动时钟：可推进的 steady 时钟，供 SafeModeGuard 确定性测试。
struct ManualClock {
    std::chrono::steady_clock::time_point now{};
};

using optimizer::service::SafeModeGuard;
using optimizer::service::SafeModeState;

bool TestSafeModeWindowTriggersWithinThreshold() {
    // 时间窗口内达阈值即触发（含正常受理交错：正常受理不复位，IPC-013 语义）。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(10000);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    if (guard.State() != SafeModeState::Normal ||
        !guard.ShouldAcceptClients()) {
        return false;
    }
    guard.OnClientRejected();            // 失败 1
    clock.now += std::chrono::milliseconds(300); // 合法 Agent 受理时段
    guard.OnClientRejected();            // 失败 2（正常受理不复位计数）
    clock.now += std::chrono::milliseconds(300);
    if (guard.State() != SafeModeState::Normal) {
        return false; // 阈值-1：仍 Normal
    }
    guard.OnClientRejected(); // 失败 3（窗口内）达阈值：进入 Safe Mode
    return guard.State() == SafeModeState::SafeMode &&
           !guard.ShouldAcceptClients() &&
           guard.CooldownRemaining() > std::chrono::milliseconds(0);
}

bool TestSafeModeWindowExpiryPreventsOldFailures() {
    // 窗口外旧失败自然过期：不参与计数（低速单点失败不会积累触发）。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(10000);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnClientRejected();
    clock.now += std::chrono::milliseconds(1000);
    guard.OnClientRejected();
    if (guard.State() != SafeModeState::Normal) {
        return false;
    }
    clock.now += std::chrono::milliseconds(10000); // 超出 5s 窗口
    guard.OnClientRejected(); // 旧失败已剪除，新失败才第 1 次
    guard.OnClientRejected();
    return guard.State() == SafeModeState::Normal &&
           guard.ShouldAcceptClients();
}

bool TestSafeModeCooldownRecoveryClearsWindow() {
    // 冷却到期自动恢复并清空窗口：恢复后旧失败不会导致瞬间再次触发。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(1000);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnClientRejected();
    guard.OnClientRejected();
    guard.OnClientRejected(); // 进入 Safe Mode
    if (guard.State() != SafeModeState::SafeMode) {
        return false;
    }
    clock.now += std::chrono::milliseconds(1500); // 冷却到期
    if (guard.State() != SafeModeState::Normal ||
        !guard.ShouldAcceptClients() ||
        guard.CooldownRemaining() != std::chrono::milliseconds(0)) {
        return false;
    }
    guard.OnClientRejected(); // 窗口已清空：仅 1 次失败，不会立即重入
    return guard.State() == SafeModeState::Normal;
}

bool TestSafeModeDisabledStaysNormal() {
    ManualClock clock;
    SafeModeGuard::Options options;
    options.enabled = false;
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    for (int i = 0; i < 6; ++i) {
        guard.OnClientRejected();
    }
    return guard.State() == SafeModeState::Normal &&
           guard.ShouldAcceptClients() &&
           guard.CooldownRemaining() == std::chrono::milliseconds(0);
}

bool TestSafeModeGuardBoundaryAndCooldown() {
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(500);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    // 冷却剩余时长随时钟推进递减。
    guard.OnClientRejected();
    guard.OnClientRejected();
    guard.OnClientRejected();
    if (guard.CooldownRemaining() <= std::chrono::milliseconds(0) ||
        guard.CooldownRemaining() > std::chrono::milliseconds(500)) {
        return false;
    }
    clock.now += std::chrono::milliseconds(300);
    const auto remaining = guard.CooldownRemaining();
    return remaining > std::chrono::milliseconds(0) &&
           remaining <= std::chrono::milliseconds(200);
}

} // namespace

int wmain() {
    int failed = 0;
    const auto run = [&failed](const wchar_t* name, bool (*test)()) {
        const bool passed = test();
        std::wcout << (passed ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
        if (!passed) {
            ++failed;
        }
    };

    run(L"parse run mode", &TestParseRunMode);
    run(L"run mode name round trip", &TestRunModeToStringRoundTrip);
    run(L"state machine legal path", &TestStateMachineLegalPath);
    run(L"state machine startup abort paths", &TestStateMachineStartupAbortPaths);
    run(L"state machine illegal transitions", &TestStateMachineIllegalTransitions);
    run(L"running report accepts stop/shutdown",
        &TestMakeStatusReportRunningAcceptsControls);
    run(L"pending/stopped report accepts nothing",
        &TestMakeStatusReportPendingAcceptsNothing);
    run(L"state to string", &TestStateToString);
    run(L"service flow status sequence", &TestServiceFlowStatusSequence);
    run(L"control stop requests stop", &TestControlStopRequestsStop);
    run(L"control interrogate repeats status",
        &TestControlInterrogateRepeatsCurrentStatus);
    run(L"control unknown code ignored", &TestControlUnknownCodeIgnored);
    run(L"control shutdown requests stop", &TestControlShutdownRequestsStop);
    run(L"dispatcher connect failure not disguised",
        &TestDispatcherConnectFailureNotDisguised);
    run(L"register control handler failure reported",
        &TestRegisterControlHandlerFailureReported);
    run(L"workload failure stops with error", &TestWorkloadFailureStopsWithError);
    run(L"report status failure reported", &TestReportStatusFailureReported);
    run(L"console bounded runs workload", &TestConsoleBoundedRunsWorkload);
    run(L"request stop ends console early", &TestRequestStopEndsConsoleEarly);
    run(L"request stop idempotent", &TestRequestStopIdempotent);
    run(L"console zero duration rejected", &TestConsoleZeroDurationRejected);
    run(L"install rejects empty names", &TestInstallRejectsEmptyNames);
    run(L"uninstall rejects empty name", &TestUninstallRejectsEmptyName);
    run(L"win32 backend rejects non-scm dispatcher",
        &TestWin32BackendRejectsNonScmDispatcher);
    run(L"safe mode window triggers within threshold",
        &TestSafeModeWindowTriggersWithinThreshold);
    run(L"safe mode window expiry prevents old failures",
        &TestSafeModeWindowExpiryPreventsOldFailures);
    run(L"safe mode cooldown recovery clears window",
        &TestSafeModeCooldownRecoveryClearsWindow);
    run(L"safe mode disabled stays normal", &TestSafeModeDisabledStaysNormal);
    run(L"safe mode boundary and cooldown", &TestSafeModeGuardBoundaryAndCooldown);
    return failed == 0 ? 0 : 1;
}
