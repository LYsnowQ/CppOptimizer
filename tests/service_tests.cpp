#include "service/service_host.hpp"
#include "service/recovery_marker.hpp"
#include "service/tray_host.hpp"
#include "service/presence.hpp"
#include "service/startup_entry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

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
using optimizer::service::ClearRecoveryMarker;
using optimizer::service::DefaultRecoveryMarkerPath;
using optimizer::service::IsRecoveryMarkerSet;
using optimizer::service::WriteRecoveryMarker;

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

bool TestConsoleResidentRunsUntilStop() {
    // 常驻（RunConsole(std::nullopt)）：无时间上限，运行到外部 RequestStop 才优雅退出。
    auto fake = std::make_shared<FakeScmBackend>();
    std::size_t ticks = 0;
    ServiceHost host(
        [&] {
            ++ticks;
            return Result<void>::Success();
        },
        MakeOptions(), fake);
    bool finished = false;
    std::thread runner([&host, &finished] {
        const auto result = host.RunConsole(std::nullopt);
        finished = result.HasValue(); // 优雅退出应返回成功
    });
    // 等待至少执行若干 tick（tick 间隔 10ms）后请求停止并回收线程。
    for (int i = 0; i < 500 && ticks < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    host.RequestStop();
    runner.join();
    return finished && host.IsStopRequested() && ticks >= 3;
}

bool TestConsoleZeroDurationRejected() {
    auto fake = std::make_shared<FakeScmBackend>();
    ServiceHost host([] { return Result<void>::Success(); }, MakeOptions(), fake);
    const auto result = host.RunConsole(std::chrono::seconds(0));
    return !result &&
           result.ErrorValue().domain == ErrorDomain::Validation;
}

// ---------- 托盘宿主（可注入图标后端，不触碰真实通知区） ----------

// 可注入托盘图标后端：记录调用序列，可注入添加失败。
class FakeTrayIconBackend : public optimizer::service::TrayIconBackend {
public:
    int addCalls = 0;
    int removeCalls = 0;
    bool failAdd = false;

    Result<void> AddIcon(void* hwnd, const std::wstring&) override {
        (void)hwnd;
        ++addCalls; // 记录尝试次数（成功/失败均计入）
        if (failAdd) {
            return Result<void>::Failure(
                Error::Validation("fake-tray", L"injected add failure"));
        }
        return Result<void>::Success();
    }

    void RemoveIcon(void*) noexcept override { ++removeCalls; }
};

bool TestTrayStartStopLifecycle() {
    auto fake = std::make_shared<FakeTrayIconBackend>();
    int exits = 0;
    optimizer::service::TrayHost tray(optimizer::service::TrayHost::Options{});
    const auto start = tray.Start([&exits] { ++exits; }, fake);
    if (!start ||
        tray.GetState() != optimizer::service::TrayHost::State::Running ||
        !tray.IsIconAdded()) {
        tray.Stop();
        return false;
    }
    tray.Stop();
    const bool clean =
        tray.GetState() == optimizer::service::TrayHost::State::Idle &&
        !tray.IsIconAdded() && exits == 0 && fake->addCalls == 1 &&
        fake->removeCalls == 1;
    if (clean) {
        // 停止后可再次启动（窗口类已注销、线程已回收）。
        const auto second = tray.Start([] {}, fake);
        if (second) {
            tray.Stop();
            return fake->addCalls == 2;
        }
    }
    return clean;
}

bool TestTrayMenuExitInvokesCallbackAndStops() {
    auto fake = std::make_shared<FakeTrayIconBackend>();
    std::atomic<int> exits{0};
    optimizer::service::TrayHost tray(optimizer::service::TrayHost::Options{});
    if (!tray.Start([&exits] { ++exits; }, fake)) {
        return false;
    }
    void* window = tray.WindowHandle();
    if (window == nullptr) {
        tray.Stop();
        return false;
    }
    // 投递退出命令（模拟右键菜单选中“退出”的 WM_COMMAND 路径；真实弹窗留人工点验）。
    const BOOL posted = ::PostMessageW(
        static_cast<HWND>(window), WM_COMMAND,
        optimizer::service::kTrayExitCommandId, 0);
    for (int i = 0; i < 1000 && exits.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    tray.Stop(); // 幂等收尾（菜单路径线程已自退出或随本调用退出）
    return posted && exits.load() == 1 && fake->addCalls == 1 &&
           fake->removeCalls >= 1 &&
           tray.GetState() == optimizer::service::TrayHost::State::Idle;
}

bool TestTrayAddFailureReported() {
    auto fake = std::make_shared<FakeTrayIconBackend>();
    fake->failAdd = true;
    optimizer::service::TrayHost tray(optimizer::service::TrayHost::Options{});
    const auto start = tray.Start([] {}, fake);
    // 添加失败：Start 报错、无图标、不遗留线程。
    if (start) {
        return false;
    }
    return fake->addCalls == 1 && fake->removeCalls == 0 &&
           tray.GetState() == optimizer::service::TrayHost::State::Idle &&
           !tray.IsIconAdded();
}

bool TestTrayDoubleStartRejected() {
    auto fake = std::make_shared<FakeTrayIconBackend>();
    optimizer::service::TrayHost tray(optimizer::service::TrayHost::Options{});
    const auto first = tray.Start([] {}, fake);
    if (!first) {
        return false;
    }
    const auto second = tray.Start([] {}, fake);
    tray.Stop();
    return !second &&
           second.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestTrayMessageObserverInvoked() {
    // 自定义消息观察（SVC-008 用）：托盘窗口收到 WM_APP 类消息时回调（UI 线程）且可拦截。
    auto fake = std::make_shared<FakeTrayIconBackend>();
    std::atomic<int> observed{0};
    optimizer::service::TrayHost::Options options;
    options.messageObserver =
        [&observed](UINT message, WPARAM, LPARAM) -> bool {
            if (message == WM_APP) {
                ++observed;
                return true; // 已处理：跳过默认处理
            }
            return false;
        };
    optimizer::service::TrayHost tray(options);
    if (!tray.Start([] {}, fake)) {
        return false;
    }
    void* window = tray.WindowHandle();
    if (window == nullptr) {
        tray.Stop();
        return false;
    }
    const BOOL posted = ::PostMessageW(static_cast<HWND>(window), WM_APP, 0, 0);
    for (int i = 0; i < 500 && observed.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    tray.Stop();
    return posted && observed.load() == 1;
}

// ---------- 宿主在场台账（纯逻辑，可注入时钟） ----------

bool TestPresenceClassify() {
    using optimizer::service::ClassifyPresence;
    using optimizer::service::PresenceState;
    std::optional<std::uint32_t> missing;
    return ClassifyPresence(missing, 15) == PresenceState::Unknown && // 未上报不伪装在场
           ClassifyPresence(std::uint32_t{0}, 15) == PresenceState::Present &&
           ClassifyPresence(std::uint32_t{14}, 15) == PresenceState::Present &&
           ClassifyPresence(std::uint32_t{15}, 15) == PresenceState::Away && // 达阈值即不在场
           ClassifyPresence(std::uint32_t{120}, 15) == PresenceState::Away &&
           ClassifyPresence(std::uint32_t{120}, 0) == PresenceState::Present && // 阈值 0 不判定
           ClassifyPresence(missing, 0) == PresenceState::Unknown;
}

bool TestPresenceTrackerSummaryRules() {
    using optimizer::service::HostPresenceTracker;
    using optimizer::service::PresenceState;
    const auto base = std::chrono::steady_clock::now();
    long elapsedMs = 0;
    HostPresenceTracker::Options options;
    options.now = [&base, &elapsedMs] {
        return base + std::chrono::milliseconds(elapsedMs);
    };
    HostPresenceTracker tracker(options);
    if (tracker.Summary() != PresenceState::Unknown ||
        tracker.ClientCount() != 0) {
        return false; // 空台账 Unknown
    }
    if (tracker.Record("a", std::uint32_t{2}) != PresenceState::Present ||
        tracker.Summary() != PresenceState::Present) {
        return false;
    }
    (void)tracker.Record("a", std::uint32_t{40}); // 同一客户端更新为不在场
    if (tracker.Summary() != PresenceState::Away) {
        return false;
    }
    (void)tracker.Record("b", std::uint32_t{3}); // 第二客户端在场：任一在场 -> Present
    if (tracker.Summary() != PresenceState::Present ||
        tracker.ClientCount() != 2) {
        return false;
    }
    // 未上报空闲的客户端记 Unknown，不影响“有人在”汇总。
    (void)tracker.Record("c", std::nullopt);
    return tracker.Summary() == PresenceState::Present &&
           tracker.ClientCount() == 3;
}

bool TestPresenceTrackerForgetEvicts() {
    using optimizer::service::HostPresenceTracker;
    using optimizer::service::PresenceState;
    const auto base = std::chrono::steady_clock::now();
    long elapsedMs = 0;
    HostPresenceTracker::Options options;
    options.now = [&base, &elapsedMs] {
        return base + std::chrono::milliseconds(elapsedMs);
    };
    options.forgetAfter = std::chrono::seconds(5);
    HostPresenceTracker tracker(options);
    (void)tracker.Record("a", std::uint32_t{1});
    if (tracker.Summary() != PresenceState::Present) {
        return false;
    }
    elapsedMs = 6000; // 超过遗忘时长：客户端移除，回到 Unknown
    return tracker.Summary() == PresenceState::Unknown &&
           tracker.ClientCount() == 0;
}

bool TestPresenceStateNames() {
    using optimizer::service::PresenceState;
    using optimizer::service::PresenceStateToString;
    return std::wstring(PresenceStateToString(PresenceState::Present)) ==
               L"Present" &&
           std::wstring(PresenceStateToString(PresenceState::Away)) == L"Away" &&
           std::wstring(PresenceStateToString(PresenceState::Unknown)) ==
               L"Unknown";
}

bool TestPresenceTrackerChangeEvents() {
    using optimizer::service::HostPresenceTracker;
    using optimizer::service::PresenceState;
    const auto base = std::chrono::steady_clock::now();
    long elapsedMs = 0;
    HostPresenceTracker::Options options;
    options.now = [&base, &elapsedMs] {
        return base + std::chrono::milliseconds(elapsedMs);
    };
    HostPresenceTracker tracker(options);
    std::vector<PresenceState> events; // 记录每次变化的“新状态”
    tracker.SetOnChange([&events](PresenceState, PresenceState to) {
        events.push_back(to);
    });
    if (tracker.Summary() != PresenceState::Unknown || !events.empty()) {
        return false; // 空台账 Unknown 且不触发
    }
    (void)tracker.Record("a", std::uint32_t{1});
    if (tracker.Summary() != PresenceState::Present || events.size() != 1 ||
        events[0] != PresenceState::Present) {
        return false; // Unknown -> Present
    }
    (void)tracker.Summary(); // 状态未变：不重复触发
    if (events.size() != 1) {
        return false;
    }
    (void)tracker.Record("a", std::uint32_t{40});
    if (tracker.Summary() != PresenceState::Away || events.size() != 2 ||
        events[1] != PresenceState::Away) {
        return false; // Present -> Away
    }
    (void)tracker.Record("b", std::uint32_t{2});
    if (tracker.Summary() != PresenceState::Present || events.size() != 3 ||
        events[2] != PresenceState::Present) {
        return false; // Away -> Present（任一在场）
    }
    (void)tracker.Record("c", std::nullopt); // Unknown 不影响汇总：无事件
    return tracker.Summary() == PresenceState::Present && events.size() == 3;
}

bool TestPresenceTimelineAppend() {
    using optimizer::service::AppendPresenceTransitionLine;
    using optimizer::service::PresenceState;
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return false;
    }
    const auto path = root /
        (L"cpo_presence_timeline_" + std::to_wstring(::GetCurrentProcessId()) +
         L".log");
    std::filesystem::remove(path, ec);
    ec.clear();
    // 空路径拒绝；追加两条；内容可读；目录当文件路径时如实失败。
    if (AppendPresenceTransitionLine({}, PresenceState::Unknown,
                                     PresenceState::Present)) {
        return false;
    }
    if (!AppendPresenceTransitionLine(path, PresenceState::Unknown,
                                      PresenceState::Present) ||
        !AppendPresenceTransitionLine(path, PresenceState::Present,
                                      PresenceState::Away)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::string line1, line2;
    std::getline(in, line1);
    std::getline(in, line2);
    const bool ok = !line1.empty() && line1.find("Unknown -> Present") !=
                        std::string::npos &&
                    line2.find("Present -> Away") != std::string::npos &&
                    !AppendPresenceTransitionLine(root, PresenceState::Away,
                                                  PresenceState::Unknown);
    std::filesystem::remove(path, ec);
    return ok;
}

bool TestEffectivePresenceAwaySeconds() {
    using optimizer::service::EffectivePresenceAwaySeconds;
    // [policy].user_away_idle_seconds = 0（不启用）-> 回退默认阈值；> 0 -> 采用政策值。
    return EffectivePresenceAwaySeconds(0, 15) == 15 &&
           EffectivePresenceAwaySeconds(5, 15) == 5 &&
           EffectivePresenceAwaySeconds(300, 15) == 300;
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

// ---------- IPC-015：离散异常触发（Native 探测异常等，锁存语义） ----------

bool TestSafeModeAnomalyLatchesBeyondCooldown() {
    // 离散异常触发即进入 Safe Mode 并锁存：即使远超冷却时长也不自动恢复。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(1000);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnAnomalyDetected();
    if (guard.State() != SafeModeState::SafeMode ||
        !guard.IsAnomalyLatched() || guard.ShouldAcceptClients()) {
        return false;
    }
    clock.now += std::chrono::milliseconds(60000); // 远超计数冷却
    return guard.State() == SafeModeState::SafeMode &&
           !guard.ShouldAcceptClients() &&
           guard.CooldownRemaining() == std::chrono::milliseconds(0) &&
           guard.IsAnomalyLatched();
}

bool TestSafeModeAnomalyClearedExplicitly() {
    // 显式 ClearAnomaly 回到 Normal 并恢复受理（异常已恢复/所有者确认）。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnAnomalyDetected();
    guard.ClearAnomaly();
    return guard.State() == SafeModeState::Normal &&
           guard.ShouldAcceptClients() &&
           !guard.IsAnomalyLatched() &&
           guard.CooldownRemaining() == std::chrono::milliseconds(0);
}

bool TestSafeModeAnomalyDisabledNoOp() {
    // enabled=false：离散异常触发不生效（恒 Normal，与计数触发一致）。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.enabled = false;
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnAnomalyDetected();
    return guard.State() == SafeModeState::Normal &&
           guard.ShouldAcceptClients() && !guard.IsAnomalyLatched();
}

bool TestSafeModeAnomalyIgnoresRejectionsWhileLatched() {
    // 锁存期间的拒绝不参与计数：ClearAnomaly 后需重新累计才触发（防锁存期"攒失败"）。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(10000);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnAnomalyDetected();
    for (int i = 0; i < 3; ++i) {
        guard.OnClientRejected(); // 锁存中：忽略
    }
    guard.ClearAnomaly();
    guard.OnClientRejected();
    guard.OnClientRejected(); // 仅 2 次（阈值 3），不应触发
    return guard.State() == SafeModeState::Normal &&
           guard.ShouldAcceptClients();
}

bool TestSafeModeClearAnomalyKeepsCountingIndependent() {
    // 计数触发进入的 Safe Mode 由冷却自动恢复；ClearAnomaly（无异常）不影响该路径。
    ManualClock clock;
    SafeModeGuard::Options options;
    options.cooldown = std::chrono::milliseconds(1000);
    options.now = [&clock] { return clock.now; };
    SafeModeGuard guard(options);
    guard.OnClientRejected();
    guard.OnClientRejected();
    guard.OnClientRejected(); // 计数触发进入 Safe Mode
    if (guard.State() != SafeModeState::SafeMode) {
        return false;
    }
    guard.ClearAnomaly(); // 无异常锁存：应无效果
    clock.now += std::chrono::milliseconds(1500); // 冷却到期
    return guard.State() == SafeModeState::Normal &&
           guard.ShouldAcceptClients() && !guard.IsAnomalyLatched();
}

// ---------- IPC-018：恢复标记（异常退出未确认，recovery-state.json） ----------

std::filesystem::path TempMarkerPath() {
    // 测试专用唯一临时路径（每次调用新建，测试结束由各用例清理）。
    static std::uint64_t counter = 0;
    ++counter;
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = std::filesystem::path(L".");
    }
    return dir / (L"cppopt_recovery_test_" +
                  std::to_wstring(::GetCurrentProcessId()) + L"_" +
                  std::to_wstring(counter) + L".json");
}

bool TestRecoveryMarkerWriteExistsClearRoundTrip() {
    const auto path = TempMarkerPath();
    if (!WriteRecoveryMarker(path)) {
        return false;
    }
    const auto set = IsRecoveryMarkerSet(path);
    if (!set || !set.Value()) {
        return false;
    }
    if (!ClearRecoveryMarker(path)) {
        return false;
    }
    const auto cleared = IsRecoveryMarkerSet(path);
    std::error_code ec;
    std::filesystem::remove(path, ec); // 兜底清理
    return cleared && !cleared.Value();
}

bool TestRecoveryMarkerAbsentFileNotSet() {
    const auto path = TempMarkerPath();
    std::error_code ec;
    std::filesystem::remove(path, ec); // 确保不存在
    const auto set = IsRecoveryMarkerSet(path);
    std::error_code ec2;
    std::filesystem::remove(path, ec2);
    return set && !set.Value();
}

bool TestRecoveryMarkerMalformedContentNotSet() {
    // 目录内其它文件（内容非本标记信封）不得被误认为恢复标记。
    const auto path = TempMarkerPath();
    {
        std::ofstream out(path, std::ios::binary);
        out << "not-a-recovery-marker\n";
    }
    const auto set = IsRecoveryMarkerSet(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return set && !set.Value();
}

} // namespace

// ---------- AGENT-A1：自启动项（fake 后端；不触碰真实注册表） ----------

class FakeStartupBackend final : public optimizer::service::StartupEntryBackend {
public:
    std::wstring stored;          // 已写入的命令行（空 = 未注册）
    bool failWrite = false;
    bool failRemove = false;
    int writeCalls = 0;
    int removeCalls = 0;

    [[nodiscard]] optimizer::common::Result<std::wstring> Read() override {
        return optimizer::common::Result<std::wstring>::Success(stored);
    }
    [[nodiscard]] optimizer::common::Result<void> Write(
        const std::wstring& quotedCommand) override {
        ++writeCalls;
        if (failWrite) {
            return optimizer::common::Result<void>::Failure(
                optimizer::common::Error::FromWin32(5u, "FakeStartupBackend::Write"));
        }
        stored = quotedCommand;
        return optimizer::common::Result<void>::Success();
    }
    [[nodiscard]] optimizer::common::Result<void> Remove() override {
        ++removeCalls;
        if (failRemove) {
            return optimizer::common::Result<void>::Failure(optimizer::common::Error::FromWin32(
                5u, "FakeStartupBackend::Remove"));
        }
        stored.clear(); // 不存在也视为成功（幂等）
        return optimizer::common::Result<void>::Success();
    }
};

bool TestStartupInstallRejectsEmptyPath() {
    FakeStartupBackend backend;
    const auto result =
        optimizer::service::InstallStartupEntry(backend, std::wstring());
    return !result && backend.writeCalls == 0 &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestStartupInstallQuotesPathAndReadsBack() {
    FakeStartupBackend backend;
    const std::wstring exe = L"C:\\Program Files\\CppOptimizer\\CppOptimizer.exe";
    if (!optimizer::service::InstallStartupEntry(backend, exe)) {
        return false;
    }
    // 后端收到的是带引号的完整命令行（路径含空格仍可正确启动）。
    if (backend.stored != L"\"" + exe + L"\"") {
        return false;
    }
    const auto read = optimizer::service::QueryStartupEntry(backend);
    return read && read.Value() == backend.stored;
}

bool TestStartupQueryUnregisteredIsEmpty() {
    FakeStartupBackend backend;
    const auto read = optimizer::service::QueryStartupEntry(backend);
    return read && read.Value().empty(); // 未注册 = 空串（Success，不是错误）
}

bool TestStartupRemoveIsIdempotent() {
    FakeStartupBackend backend;
    (void)optimizer::service::InstallStartupEntry(backend, L"C:\\x\\a.exe");
    const auto first = optimizer::service::RemoveStartupEntry(backend);
    const auto second = optimizer::service::RemoveStartupEntry(backend); // 幂等
    return first && second && backend.stored.empty() && backend.removeCalls == 2;
}

bool TestStartupBackendFailureIsReported() {
    FakeStartupBackend backend;
    backend.failWrite = true;
    const auto write = optimizer::service::InstallStartupEntry(backend, L"C:\\x\\a.exe");
    backend.failRemove = true;
    const auto remove = optimizer::service::RemoveStartupEntry(backend);
    // 失败如实上报（不伪成功）且域为 Win32。
    return !write && write.ErrorValue().domain == optimizer::common::ErrorDomain::Win32 &&
           !remove &&
           remove.ErrorValue().domain == optimizer::common::ErrorDomain::Win32;
}

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
    run(L"console resident runs until stop", &TestConsoleResidentRunsUntilStop);
    run(L"tray start stop lifecycle", &TestTrayStartStopLifecycle);
    run(L"tray menu exit invokes callback and stops", &TestTrayMenuExitInvokesCallbackAndStops);
    run(L"tray add failure reported", &TestTrayAddFailureReported);
    run(L"tray double start rejected", &TestTrayDoubleStartRejected);
    run(L"tray message observer invoked", &TestTrayMessageObserverInvoked);
    run(L"presence classify", &TestPresenceClassify);
    run(L"presence tracker summary rules", &TestPresenceTrackerSummaryRules);
    run(L"presence tracker forget evicts", &TestPresenceTrackerForgetEvicts);
    run(L"presence tracker change events", &TestPresenceTrackerChangeEvents);
    run(L"presence timeline append", &TestPresenceTimelineAppend);
    run(L"presence state names", &TestPresenceStateNames);
    run(L"presence effective away seconds", &TestEffectivePresenceAwaySeconds);
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
    run(L"safe mode anomaly latches beyond cooldown",
        &TestSafeModeAnomalyLatchesBeyondCooldown);
    run(L"safe mode anomaly cleared explicitly", &TestSafeModeAnomalyClearedExplicitly);
    run(L"safe mode anomaly disabled no-op", &TestSafeModeAnomalyDisabledNoOp);
    run(L"safe mode anomaly ignores rejections while latched",
        &TestSafeModeAnomalyIgnoresRejectionsWhileLatched);
    run(L"safe mode clear anomaly keeps counting independent",
        &TestSafeModeClearAnomalyKeepsCountingIndependent);
    run(L"recovery marker write exists clear round trip",
        &TestRecoveryMarkerWriteExistsClearRoundTrip);
    run(L"recovery marker absent file not set", &TestRecoveryMarkerAbsentFileNotSet);
    run(L"recovery marker malformed content not set",
        &TestRecoveryMarkerMalformedContentNotSet);
    run(L"startup install rejects empty path", &TestStartupInstallRejectsEmptyPath);
    run(L"startup install quotes path and reads back",
        &TestStartupInstallQuotesPathAndReadsBack);
    run(L"startup query unregistered is empty", &TestStartupQueryUnregisteredIsEmpty);
    run(L"startup remove is idempotent", &TestStartupRemoveIsIdempotent);
    run(L"startup backend failure is reported",
        &TestStartupBackendFailureIsReported);
    return failed == 0 ? 0 : 1;
}
