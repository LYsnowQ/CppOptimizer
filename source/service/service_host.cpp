#include "service/service_host.hpp"

#include <utility>
#include <vector>

namespace optimizer::service {

namespace {

// 控制台信号 -> 当前控制台宿主实例。控制台模式为前台单实例
// （同一时刻至多一个 RunConsole），静态指针受互斥保护，避免信号回调访问已析构实例。
ServiceHost* gActiveConsoleHost = nullptr;
std::mutex gConsoleHostMutex;

// 控制台信号处理器（专用线程）：请求优雅停止并吞掉信号，等待宿主自行退出。
BOOL WINAPI ConsoleSignalHandler(DWORD eventType) noexcept {
    if (eventType == CTRL_C_EVENT || eventType == CTRL_BREAK_EVENT ||
        eventType == CTRL_CLOSE_EVENT || eventType == CTRL_SHUTDOWN_EVENT) {
        std::lock_guard<std::mutex> lock(gConsoleHostMutex);
        if (gActiveConsoleHost != nullptr) {
            gActiveConsoleHost->RequestStop();
        }
        return TRUE; // 已处理：不执行默认终止，等待优雅退出
    }
    return FALSE;
}

// 本项目枚举 -> SERVICE_STATUS.dwCurrentState 常量。
DWORD ToScmState(ServiceState state) noexcept {
    switch (state) {
        case ServiceState::StartPending:
            return SERVICE_START_PENDING;
        case ServiceState::Running:
            return SERVICE_RUNNING;
        case ServiceState::StopPending:
            return SERVICE_STOP_PENDING;
        case ServiceState::Stopped:
            return SERVICE_STOPPED;
        case ServiceState::Unknown:
            break;
    }
    return SERVICE_STOPPED;
}

// 真实 Win32 SCM 后端。服务主函数与控制回调均为静态 trampoline：
// StartServiceCtrlDispatcherW 无 context 参数，用文件级静态指针转发
// （单服务进程，与 Windows 官方服务样例一致）；控制回调由
// RegisterServiceCtrlHandlerExW 的 lpContext 携带 this，无需静态状态。
class Win32ScmBackend final : public ScmBackend {
public:
    common::Result<void> RegisterControlHandler(
        std::wstring_view serviceName,
        std::function<void(std::uint32_t)> handler) noexcept override {
        controlHandler_ = std::move(handler);
        const std::wstring name(serviceName);
        statusHandle_ = ::RegisterServiceCtrlHandlerExW(
            name.c_str(), &ControlHandlerTrampoline, this);
        if (statusHandle_ == nullptr) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(), "RegisterServiceCtrlHandlerExW"));
        }
        return common::Result<void>::Success();
    }

    common::Result<void> ReportStatus(
        const ServiceStatusReport& report) noexcept override {
        if (statusHandle_ == nullptr) {
            return common::Result<void>::Failure(common::Error::Validation(
                "ReportStatus", L"未注册控制回调，无法上报状态"));
        }
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = ToScmState(report.state);
        status.dwControlsAccepted = report.controlsAccepted;
        status.dwWin32ExitCode = report.win32ExitCode;
        status.dwCheckPoint = report.checkPoint;
        status.dwWaitHint = report.waitHintMs;
        if (!::SetServiceStatus(statusHandle_, &status)) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(), "SetServiceStatus"));
        }
        return common::Result<void>::Success();
    }

    common::Result<void> RunServiceDispatcher(
        std::wstring_view serviceName,
        std::function<common::Result<void>(std::wstring_view)>
            serviceMain) noexcept override {
        serviceName_ = std::wstring(serviceName);
        serviceMain_ = std::move(serviceMain);
        gDispatcherBackend = this;
        const SERVICE_TABLE_ENTRYW table[] = {
            {serviceName_.data(), &ServiceMainTrampoline},
            {nullptr, nullptr}};
        const BOOL dispatched = ::StartServiceCtrlDispatcherW(table);
        gDispatcherBackend = nullptr;
        if (!dispatched) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(), "StartServiceCtrlDispatcherW"));
        }
        return common::Result<void>::Success();
    }

private:
    // SCM 服务主函数 trampoline：经静态指针转发到实例的 serviceMain_。
    static void WINAPI ServiceMainTrampoline(DWORD, LPWSTR*) noexcept {
        Win32ScmBackend* backend = gDispatcherBackend;
        if (backend == nullptr) {
            return;
        }
        try {
            // 失败已在 RunServiceMain 内上报（SERVICE_STOPPED + 退出码），
            // 此处不跨 SCM 回调抛异常。
            (void)backend->serviceMain_(backend->serviceName_);
        } catch (...) {
        }
    }

    // 控制回调 trampoline：lpContext 为后端实例，无需静态状态。
    static DWORD WINAPI ControlHandlerTrampoline(
        DWORD control, DWORD eventType, LPVOID eventData, LPVOID context) noexcept {
        auto* backend = static_cast<Win32ScmBackend*>(context);
        if (backend != nullptr && backend->controlHandler_) {
            try {
                backend->controlHandler_(control);
            } catch (...) {
            }
        }
        (void)eventType;
        (void)eventData;
        return NO_ERROR;
    }

    static Win32ScmBackend* gDispatcherBackend;

    std::wstring serviceName_;
    std::function<common::Result<void>(std::wstring_view)> serviceMain_;
    std::function<void(std::uint32_t)> controlHandler_;
    SERVICE_STATUS_HANDLE statusHandle_ = nullptr;
};

Win32ScmBackend* Win32ScmBackend::gDispatcherBackend = nullptr;

} // namespace

const wchar_t* RunModeToString(RunMode mode) noexcept {
    switch (mode) {
        case RunMode::Console:
            return L"console";
        case RunMode::Service:
            return L"service";
        case RunMode::Install:
            return L"install";
        case RunMode::Uninstall:
            return L"uninstall";
    }
    return L"unknown";
}

std::optional<RunMode> ParseRunMode(std::wstring_view text) noexcept {
    // ASCII 大小写不敏感比较（与项目其他 CLI 解析语义一致）。
    const auto eq = [](std::wstring_view a, std::wstring_view b) noexcept {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const wchar_t ca =
                a[i] >= L'A' && a[i] <= L'Z' ? a[i] - L'A' + L'a' : a[i];
            const wchar_t cb =
                b[i] >= L'A' && b[i] <= L'Z' ? b[i] - L'A' + L'a' : b[i];
            if (ca != cb) {
                return false;
            }
        }
        return true;
    };
    if (eq(text, L"console")) {
        return RunMode::Console;
    }
    if (eq(text, L"service")) {
        return RunMode::Service;
    }
    if (eq(text, L"install")) {
        return RunMode::Install;
    }
    if (eq(text, L"uninstall")) {
        return RunMode::Uninstall;
    }
    return std::nullopt;
}

const wchar_t* StateToString(ServiceState state) noexcept {
    switch (state) {
        case ServiceState::Unknown:
            return L"unknown";
        case ServiceState::StartPending:
            return L"start_pending";
        case ServiceState::Running:
            return L"running";
        case ServiceState::StopPending:
            return L"stop_pending";
        case ServiceState::Stopped:
            return L"stopped";
    }
    return L"unknown";
}

ServiceStatusReport MakeStatusReport(ServiceState state,
                                     std::uint32_t win32ExitCode,
                                     std::uint32_t checkPoint,
                                     std::uint32_t waitHintMs) noexcept {
    ServiceStatusReport report;
    report.state = state;
    report.win32ExitCode = win32ExitCode;
    report.checkPoint = checkPoint;
    report.waitHintMs = waitHintMs;
    if (state == ServiceState::Running) {
        report.controlsAccepted = kServiceControlsAccepted;
    }
    return report;
}

common::Result<void> ValidateStateTransition(ServiceState from,
                                             ServiceState to) noexcept {
    bool legal = false;
    switch (from) {
        case ServiceState::Unknown:
            legal = (to == ServiceState::StartPending);
            break;
        case ServiceState::StartPending:
            legal = (to == ServiceState::Running ||
                     to == ServiceState::StopPending ||
                     to == ServiceState::Stopped);
            break;
        case ServiceState::Running:
            legal = (to == ServiceState::StopPending ||
                     to == ServiceState::Stopped);
            break;
        case ServiceState::StopPending:
            legal = (to == ServiceState::Stopped);
            break;
        case ServiceState::Stopped:
            legal = false; // 终态：服务进程退出，无后继
            break;
    }
    if (legal) {
        return common::Result<void>::Success();
    }
    return common::Result<void>::Failure(common::Error::Validation(
        "ValidateStateTransition",
        std::wstring(L"非法服务状态转移: ") + StateToString(from) + L" -> " +
            StateToString(to)));
}

std::shared_ptr<ScmBackend> CreateWin32ScmBackend() {
    return std::make_shared<Win32ScmBackend>();
}

common::Result<void> InstallService(const ServiceIdentity& identity) noexcept {
    if (identity.name.empty() || identity.displayName.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "InstallService", L"服务名与显示名不能为空"));
    }
    // exe 路径：为空则取当前模块路径（两阶段查询，处理长度变化）。
    std::wstring exePath = identity.executablePath;
    if (exePath.empty()) {
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;) {
            const DWORD copied = ::GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0) {
                return common::Result<void>::Failure(common::Error::FromWin32(
                    ::GetLastError(), "GetModuleFileNameW"));
            }
            if (copied < buffer.size()) {
                exePath.assign(buffer.data(), copied);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
    }
    // ImagePath 带引号：路径含空格时 SCM 解析依赖引号。
    const std::wstring imagePath = L"\"" + exePath + L"\"";

    common::UniqueServiceHandle scm(
        ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (!scm) {
        return common::Result<void>::Failure(
            common::Error::FromWin32(::GetLastError(), "OpenSCManagerW"));
    }
    // 服务句柄最小权限：SERVICE_CHANGE_CONFIG（仅用于设置描述）。
    common::UniqueServiceHandle svc(::CreateServiceW(
        scm.Get(), identity.name.c_str(), identity.displayName.c_str(),
        SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, imagePath.c_str(), nullptr,
        nullptr, nullptr, nullptr, nullptr));
    if (!svc) {
        return common::Result<void>::Failure(
            common::Error::FromWin32(::GetLastError(), "CreateServiceW"));
    }
    // 描述为装饰性字段：写入失败不阻断安装，仅留诊断输出。
    if (!identity.description.empty()) {
        SERVICE_DESCRIPTIONW description{};
        description.lpDescription =
            const_cast<LPWSTR>(identity.description.c_str());
        if (!::ChangeServiceConfig2W(svc.Get(), SERVICE_CONFIG_DESCRIPTION,
                                     &description)) {
            ::OutputDebugStringW(
                L"CppOptimizer: ChangeServiceConfig2W(description) failed");
        }
    }
    return common::Result<void>::Success();
}

common::Result<void> UninstallService(std::wstring_view name) noexcept {
    if (name.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "UninstallService", L"服务名不能为空"));
    }
    common::UniqueServiceHandle scm(
        ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        return common::Result<void>::Failure(
            common::Error::FromWin32(::GetLastError(), "OpenSCManagerW"));
    }
    common::UniqueServiceHandle svc(
        ::OpenServiceW(scm.Get(), std::wstring(name).c_str(), DELETE));
    if (!svc) {
        return common::Result<void>::Failure(
            common::Error::FromWin32(::GetLastError(), "OpenServiceW"));
    }
    if (!::DeleteService(svc.Get())) {
        return common::Result<void>::Failure(
            common::Error::FromWin32(::GetLastError(), "DeleteService"));
    }
    return common::Result<void>::Success();
}

ServiceHost::ServiceHost(Workload workload, Options options,
                         std::shared_ptr<ScmBackend> backend)
    : workload_(std::move(workload)),
      options_(std::move(options)),
      backend_(std::move(backend)) {}

ServiceHost::~ServiceHost() noexcept = default;

void ServiceHost::RequestStop() noexcept {
    stopRequested_.store(true);
    cv_.notify_all();
}

bool ServiceHost::IsStopRequested() const noexcept {
    return stopRequested_.load();
}

common::Result<void> ServiceHost::RunConsole(
    std::optional<std::chrono::seconds> boundedFor) noexcept {
    if (boundedFor && boundedFor->count() <= 0) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RunConsole", L"boundedFor 必须为正秒数"));
    }
    {
        std::lock_guard<std::mutex> lock(gConsoleHostMutex);
        gActiveConsoleHost = this;
    }
    if (!::SetConsoleCtrlHandler(&ConsoleSignalHandler, TRUE)) {
        std::lock_guard<std::mutex> lock(gConsoleHostMutex);
        gActiveConsoleHost = nullptr;
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "SetConsoleCtrlHandler"));
    }
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt;
    if (boundedFor) {
        deadline = std::chrono::steady_clock::now() + *boundedFor;
    }
    const auto loop = RunLoop(deadline);
    {
        std::lock_guard<std::mutex> lock(gConsoleHostMutex);
        gActiveConsoleHost = nullptr;
    }
    // 先清空实例指针再移除处理器：避免处理器在宿主析构后仍持有悬空指针。
    ::SetConsoleCtrlHandler(&ConsoleSignalHandler, FALSE);
    return loop;
}

common::Result<void> ServiceHost::RunService() noexcept {
    // SCM 分发循环阻塞直到服务停止；RunServiceMain 失败原样上报。
    return backend_->RunServiceDispatcher(
        options_.identity.name,
        [this](std::wstring_view name) { return RunServiceMain(name); });
}

common::Result<void> ServiceHost::RunServiceMain(
    std::wstring_view serviceName) noexcept {
    // SCM 规则：注册控制回调必须先于其他工作，且尽快上报状态。
    const auto registered = backend_->RegisterControlHandler(
        serviceName, [this](std::uint32_t controlCode) {
            HandleControl(controlCode);
        });
    if (!registered) {
        return registered;
    }
    if (const auto start = Transit(ServiceState::StartPending, 0, 1,
                                   kWaitHintMs);
        !start) {
        return start;
    }
    if (const auto running = Transit(ServiceState::Running); !running) {
        return running;
    }
    const auto loop = RunLoop(std::nullopt);
    const std::uint32_t exitCode = loop ? 0u : 1u;
    if (const auto stopping = Transit(ServiceState::StopPending, 0, 2,
                                      kWaitHintMs);
        !stopping) {
        return stopping;
    }
    if (const auto stopped = Transit(ServiceState::Stopped, exitCode);
        !stopped) {
        return stopped;
    }
    return loop;
}

common::Result<void> ServiceHost::RunLoop(
    std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
    for (;;) {
        if (stopRequested_.load()) {
            return common::Result<void>::Success();
        }
        if (deadline &&
            std::chrono::steady_clock::now() >= *deadline) {
            return common::Result<void>::Success();
        }
        if (const auto tick = workload_(); !tick) {
            return tick; // 负载失败：停止循环并向上报告
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, options_.tickInterval,
                     [this] { return stopRequested_.load(); });
    }
}

common::Result<void> ServiceHost::Transit(ServiceState next,
                                          std::uint32_t win32ExitCode,
                                          std::uint32_t checkPoint,
                                          std::uint32_t waitHintMs) noexcept {
    const auto from = currentState_.load();
    if (const auto validated = ValidateStateTransition(from, next);
        !validated) {
        return validated;
    }
    currentState_.store(next);
    return backend_->ReportStatus(
        MakeStatusReport(next, win32ExitCode, checkPoint, waitHintMs));
}

void ServiceHost::HandleControl(std::uint32_t controlCode) noexcept {
    if (controlCode == SERVICE_CONTROL_STOP ||
        controlCode == SERVICE_CONTROL_SHUTDOWN) {
        RequestStop();
    } else if (controlCode == SERVICE_CONTROL_INTERROGATE) {
        // 应答重报在控制回调线程执行；失败仅留诊断，不跨回调抛异常。
        if (const auto reported = ReReportCurrentStatus(); !reported) {
            ::OutputDebugStringW(
                L"CppOptimizer: ReReportCurrentStatus failed");
        }
    }
}

common::Result<void> ServiceHost::ReReportCurrentStatus() noexcept {
    const auto state = currentState_.load();
    if (state == ServiceState::Unknown || state == ServiceState::Stopped) {
        // 未开始上报或已停止：无需应答。
        return common::Result<void>::Success();
    }
    return backend_->ReportStatus(MakeStatusReport(state));
}

SafeModeGuard::SafeModeGuard(Options options)
    : options_(std::move(options)) {}

void SafeModeGuard::PruneExpired() noexcept {
    const auto cutoff = options_.now() - options_.countingWindow;
    auto& times = failureTimes_;
    while (!times.empty() && times.front() <= cutoff) {
        times.erase(times.begin());
    }
}

void SafeModeGuard::Refresh() noexcept {
    // 离散异常锁存：不随冷却流逝自动清除（停留由 ClearAnomaly 决定），也不做剪枝。
    if (anomalyLatched_) {
        return;
    }
    if (safeMode_) {
        // 冷却到期自动回到 Normal 并清空窗口（避免恢复瞬间因窗口内旧失败立即再触发）。
        if (options_.now() >= cooldownUntil_) {
            safeMode_ = false;
            failureTimes_.clear();
        }
        return;
    }
    // Normal：剪掉窗口外失败，防止长期低速失败无限积累后“越界触发”。
    PruneExpired();
}

SafeModeState SafeModeGuard::State() noexcept {
    Refresh();
    if (anomalyLatched_) {
        return SafeModeState::SafeMode;
    }
    return safeMode_ ? SafeModeState::SafeMode : SafeModeState::Normal;
}

bool SafeModeGuard::ShouldAcceptClients() noexcept {
    Refresh();
    return !options_.enabled || (!safeMode_ && !anomalyLatched_);
}

std::chrono::milliseconds SafeModeGuard::CooldownRemaining() noexcept {
    Refresh();
    // 锁存异常无冷却语义（停留由 ClearAnomaly 决定），恒 0；计数触发的冷却随时钟递减。
    if (anomalyLatched_ || !safeMode_) {
        return std::chrono::milliseconds(0);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        cooldownUntil_ - options_.now());
    return remaining.count() > 0 ? remaining : std::chrono::milliseconds(0);
}

void SafeModeGuard::OnClientRejected() noexcept {
    if (!options_.enabled) {
        return;
    }
    Refresh(); // 计数状态随时间推进：冷却到期先自动清窗，再判断是否累计
    if (safeMode_ || anomalyLatched_) {
        return; // 已在计数 Safe Mode 冷却期；或异常锁存中（已暂停受理，不累计）
    }
    // 时间窗口计数：记录时间戳，剪枝后窗口内失败数达阈值即进入 Safe Mode。
    failureTimes_.push_back(options_.now());
    PruneExpired();
    if (failureTimes_.size() >= options_.failuresToEnter) {
        safeMode_ = true;
        cooldownUntil_ = options_.now() + options_.cooldown;
    }
}

void SafeModeGuard::OnAnomalyDetected() noexcept {
    if (!options_.enabled) {
        return;
    }
    anomalyLatched_ = true; // 持续状态异常：锁存保持直到 ClearAnomaly
}

void SafeModeGuard::ClearAnomaly() noexcept {
    anomalyLatched_ = false; // 只清异常锁存；计数冷却状态由各自规则自理
}

bool SafeModeGuard::IsAnomalyLatched() const noexcept {
    return anomalyLatched_;
}

} // namespace optimizer::service
