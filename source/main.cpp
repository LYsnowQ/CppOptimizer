#include "audit/audit_log.hpp"
#include "common/console_output.hpp"
#include "common/error.hpp"
#include "activity/user_activity.hpp"
#include "activity/raw_input.hpp"
#include "config/config_manager.hpp"
#include "ipc/ipc_facts.hpp"
#include "ipc/ipc_credentials.hpp"
#include "ipc/ipc_protocol.hpp"
#include "ipc/ipc_session.hpp"
#include "ipc/ipc_transport.hpp"
#include "logger/logger.hpp"
#include "memory/memory_tuner.hpp"
#include "memory/memory_cleaner.hpp"
#include "memory/working_set.hpp"
#include "metrics/memory_metrics.hpp"
#include "metrics/pdh_metrics.hpp"
#include "platform/native_api.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_executor.hpp"
#include "activity/user_activity.hpp"
#include "platform/native_api.hpp"
#include "policy/safety_gates.hpp"
#include "power/power_locker.hpp"
#include "priority/priority_booster.hpp"
#include "process/process_watcher.hpp"
#include "service/service_host.hpp"
#include "service/startup_entry.hpp"
#include "service/agent_form.hpp"
#include "service/host_instance.hpp"
#include "service/scheduled_task.hpp"
#include "service/recovery_marker.hpp"
#include "service/tray_host.hpp"
#include "service/presence.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 每用户数据目录下的审计文件路径（定义见本文件后部）。
std::filesystem::path DefaultAuditLogPath() noexcept;

// 冷却台账路径（每用户文件；定义见本文件后部）。
std::filesystem::path DefaultCooldownLedgerPath() noexcept;

// 动作日记路径（每用户文件；定义见本文件后部）。
std::filesystem::path DefaultJournalPath() noexcept;

// 动作审计（定义见本文件后部）：供本文件前部的各命令实现调用。
void AuditAction(const char* operationId, optimizer::audit::RiskLevel risk,
                 const std::string& target, bool ok,
                 const std::string& detail, const std::string& postState);
void AuditAgentFormAction(optimizer::service::AgentForm form, bool install,
                          bool ok);
void JournalAction(const char* operationId, const std::string& target, bool ok,
                   optimizer::audit::JournalPhase phase,
                   const std::string& detail);
std::string DescribeAgentFormsSnapshot();

// 作用域错误行（替代宽字符标准错误流）：链式拼接后在临时对象析构时按 stderr 双路径一次性写出。
// 存在的理由：宽字符标准错误流在默认 C locale 下遇非 ASCII（本地化 Win32 错误消息）会进入 failbit
// 并丢弃之后的所有输出，造成“错误原因静默丢失”；本类保证消息完整，且重定向 stderr 时仍为 UTF-8 字节。
// 用法与原宽字符标准错误流相同（内容里需自行带换行符）。
class ErrorLine {
public:
    ErrorLine() = default;
    ErrorLine(const ErrorLine&) = delete;
    ErrorLine& operator=(const ErrorLine&) = delete;

    template <typename T>
    ErrorLine& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

    ~ErrorLine() {
        optimizer::common::WriteConsoleError(stream_.str());
    }

private:
    std::wostringstream stream_;
};

// CFG-006：[memory].query_enabled 门禁——配置显式关闭时**拒绝**系统内存查询并报明原因（不静默降级）。
// 只由已消费的配置影响判定：未给配置 = 默认放行（零回归）；返回 false 表示已拒绝，调用方应立即返回 2。
bool EnsureMemoryQueryAllowed(
    const std::optional<optimizer::config::ConfigSnapshot>& config) {
    if (config && !config->memory.queryEnabled) {
        ErrorLine{} << L"  memory query disabled by config ([memory].query_enabled = false)\n"
                    << L"  hint   : set query_enabled = true, or drop the config"
                    << L" argument to use the built-in default\n";
        return false;
    }
    return true;
}

// 严格无符号十进制解析：拒绝空输入、前缀垃圾与尾随非数字。
// 仅在完全解析成功时写入 out，保持 CLI 解析严格。
bool ParseUint32(std::wstring_view text, std::uint32_t& out) noexcept {
    const std::wstring copy(text);
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != L'\0') {
        return false; // 非数字或含尾随字符
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false; // 数值超出 uint32_t 范围
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

// ASCII 大小写不敏感比较（A-Z 折叠为 a-z，无区域依赖；
// 与进程名匹配/配置解析的折叠语义一致）。
bool AsciiEqualsIgnoreCaseW(std::wstring_view a, std::wstring_view b) noexcept {
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
}

int RunStatus(const std::wstring& configPath) {
    // CFG-006：可选 [config.toml]——仅在显式给出时读配置（不给则与既有行为一致）。
    std::optional<optimizer::config::ConfigSnapshot> config;
    if (!configPath.empty()) {
        auto loaded = optimizer::config::LoadConfig(configPath);
        if (!loaded) {
            const auto& error = loaded.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        config = loaded.Value();
        if (!EnsureMemoryQueryAllowed(config)) {
            return 2;
        }
    }
    const auto result = optimizer::memory::QueryMemoryStatus();
    if (!result) {
        const auto& error = result.ErrorValue();
        ErrorLine{} << L"  memory snapshot : failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& status = result.Value();
    const auto now = std::chrono::steady_clock::now();
    const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - status.sampledAt)
                           .count();
    constexpr auto kStatusFreshnessWindow = std::chrono::seconds(30);

    std::wcout << L"Memory snapshot (read-only, single query)\n";
    std::wcout << L"  total      : "
               << optimizer::memory::FormatBytes(status.totalPhysicalBytes) << L" ("
               << status.totalPhysicalBytes << L" bytes)\n";
    std::wcout << L"  available  : "
               << optimizer::memory::FormatBytes(status.availablePhysicalBytes) << L" ("
               << status.availablePhysicalBytes << L" bytes)\n";
    std::wcout << L"  used       : "
               << optimizer::memory::FormatBytes(status.usedPhysicalBytes) << L" ("
               << status.usedPhysicalBytes << L" bytes)\n";
    std::wcout << L"  load       : " << status.memoryLoadPercent << L"%\n";
    std::wcout << L"  age        : " << ageMs << L" ms (fresh: "
               << (optimizer::memory::IsSnapshotFresh(
                       status.sampledAt, now, kStatusFreshnessWindow)
                       ? L"yes"
                       : L"no")
               << L")\n";
    return 0;
}

int RunObserve(std::wstring_view secondsText, std::wstring_view thresholdText,
               const std::wstring& configPath = {}) {
    // CFG-006：可选 [config.toml]——与 --status 同口径（显式给出才读，关闭则拒绝查询）。
    if (!configPath.empty()) {
        auto loaded = optimizer::config::LoadConfig(configPath);
        if (!loaded) {
            const auto& error = loaded.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        const std::optional<optimizer::config::ConfigSnapshot> config =
            loaded.Value();
        if (!EnsureMemoryQueryAllowed(config)) {
            return 2;
        }
    }
    // 前台、有界、用户主动发起的观测：无后台线程、无周期任务、无系统写入。
    // 属指标采样而非清理轮询；自动内存清理的红区规则不适用。
    constexpr std::uint32_t kMaxSeconds = 60;
    constexpr std::uint32_t kDefaultLowLoadThreshold = 50;
    std::uint32_t seconds = 0;
    if (!ParseUint32(secondsText, seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --observe seconds must be in 1.." << kMaxSeconds << L"\n";
        return 2;
    }

    // 可选低负载阈值 0..100，默认 50。参数解析仅在命令层；纯函数将再次校验范围。
    std::uint32_t threshold = kDefaultLowLoadThreshold;
    if (!thresholdText.empty()) {
        std::uint32_t parsed = 0;
        if (!ParseUint32(thresholdText, parsed) || parsed > 100) {
            ErrorLine{} << L"  --observe threshold must be in 0..100\n";
            return 2;
        }
        threshold = parsed;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::vector<optimizer::metrics::MemorySample> samples;
    samples.reserve(seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = optimizer::memory::QueryMemoryStatus();
        if (!status) {
            const auto& error = status.ErrorValue();
            ErrorLine{} << L"  memory sample failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        samples.push_back(optimizer::metrics::MemorySample{
            status.Value().memoryLoadPercent,
            status.Value().availablePhysicalBytes});
        if (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    auto report = optimizer::metrics::AggregateMemoryWindow(samples);
    if (!report) {
        const auto& error = report.ErrorValue();
        ErrorLine{} << L"  window aggregation failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    auto share = optimizer::metrics::ShareOfLoadBelow(samples, threshold);
    if (!share) {
        const auto& error = share.ErrorValue();
        ErrorLine{} << L"  low-load share failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& r = report.Value();
    std::wcout << L"Memory observation window (read-only, foreground, " << seconds
               << L" s)\n";
    std::wcout << L"  samples      : " << r.sampleCount << L"\n";
    std::wcout << L"  load %       : min " << r.minLoadPercent << L" / avg "
               << r.avgLoadPercent << L" / max " << r.maxLoadPercent << L"\n";
    std::wcout << L"  available    : min "
               << optimizer::memory::FormatBytes(r.minAvailableBytes) << L" / max "
               << optimizer::memory::FormatBytes(r.maxAvailableBytes) << L"\n";
    std::wcout << L"  load < " << threshold << L"%   : " << share.Value()
               << L"% of samples\n";
    return 0;
}

// --gates [config.toml]：危险能力的**门禁诊断**（只读）。逐能力列出六道门的状态与首个阻塞门；
// 不执行任何动作、不改变任何门禁状态。R2/R3 的真实动作还额外要求隔离环境（危险操作策略）。
// 权限与环境门的**真实只读探测**（电池 / OS 支持 / 远程会话 / 锁屏 / 交互会话）：
// 任一项取不到即视为“未知”，门禁不得通过（不得把未知当安全）。
optimizer::policy::EnvironmentFacts GatherEnvironmentFacts() {
    optimizer::policy::EnvironmentFacts facts;
    bool osSupported = false;
    bool osKnown = false;
    if (const auto version = optimizer::platform::QueryOsVersion(); version) {
        osSupported =
            optimizer::platform::ClassifyOsSupport(version.Value(),
                                                  optimizer::platform::IsNativeX64()) ==
            optimizer::platform::OsSupport::Supported;
        osKnown = true;
    }
    bool batteryKnown = false;
    bool onBattery = false;
    if (const auto battery = optimizer::platform::QueryOnBatteryPower(); battery) {
        onBattery = battery.Value();
        batteryKnown = true;
    }
    bool sessionKnown = false;
    bool remote = false;
    bool locked = false;
    bool interactive = false;
    if (auto probe = optimizer::activity::CreateWin32SessionProbe(); probe != nullptr) {
        if (const auto context = probe->Query(); context) {
            remote = context.Value().remoteSession;
            locked = context.Value().locked;
            // 交互会话：查询成功即视为已识别（WTS 查询失败会整样本降级，见 activity 契约）。
            interactive = true;
            sessionKnown = true;
        }
    }
    facts.factsKnown = osKnown && batteryKnown && sessionKnown;
    facts.osSupported = osSupported;
    facts.onBattery = onBattery;
    facts.remoteSession = remote;
    facts.sessionLocked = locked;
    facts.interactiveSession = interactive;
    return facts;
}

// 内存清理能力的门禁事实（`--memory-clean` 的 dry-run 与真实执行路径**共用同一口径**：
// 两处各写一份会让“诊断说放行、执行却拒绝”成为可能）。
struct MemoryCleanGateContext {
    optimizer::policy::EnvironmentFacts environment{};
    optimizer::policy::GateInputs inputs{};
    optimizer::policy::GateEvaluation gates{};
    bool auditWritable = false;
};

MemoryCleanGateContext GatherMemoryCleanGates(bool acknowledged,
                                              bool configEnabled) {
    MemoryCleanGateContext context;
    context.environment = GatherEnvironmentFacts();
    context.auditWritable = static_cast<bool>(
        optimizer::audit::ProbeAuditWritable(DefaultAuditLogPath()));
    context.inputs.compileTime = optimizer::policy::MemoryCleanCompiledIn();
    context.inputs.config = configEnabled;
    context.inputs.commandLine = acknowledged; // 动作特定确认参数（显式动作）
    context.inputs.permissionAndEnvironment =
        optimizer::policy::EvaluateEnvironmentGate(context.environment);
    context.inputs.audit = context.auditWritable;
    // 冷却门：读真实台账（每用户文件）；无执行记录 -> 放行。
    const auto ledger =
        optimizer::policy::ReadCooldownLedger(DefaultCooldownLedgerPath());
    context.inputs.cooldown =
        ledger && optimizer::policy::EvaluateCooldownGate(
                      ledger.Value(), optimizer::policy::kMemoryCleanCapabilityId,
                      static_cast<std::int64_t>(std::time(nullptr)),
                      std::chrono::duration_cast<std::chrono::seconds>(
                          optimizer::policy::kDefaultCooldown));
    context.gates = optimizer::policy::EvaluateGates(context.inputs);
    return context;
}

std::wstring WidenAscii(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

// `--memory-clean <config> --execute-self-trim`：**本进程**工作集修剪的真实执行路径（R1）。
// 契约（与 guides/31 §3/§5 一致）：
// - **六道门全通才执行**，否则只打印**首个阻塞门**并拒绝（exit 2、零系统调用）；
// - **审计不可用即拒绝**（真实动作前审计链必须可用，不只看状态）；
// - 每次最多一次、单步、无循环、不提权；作用目标固定为本进程；
// - 成功才写冷却台账（失败不得写，否则会把“没做成”算成冷却）；
// - 三段日记照写，`state` 段记录可量化读数（修剪前/后工作集字节）。
int RunMemoryCleanSelfTrim(
    const std::optional<std::filesystem::path>& configPath,
    optimizer::config::CleanLevel configuredMax, bool acknowledged) {
    const std::string target = configPath.has_value() ? "config" : "defaults";
    const auto context = GatherMemoryCleanGates(
        acknowledged, configuredMax != optimizer::config::CleanLevel::None);
    if (!context.gates.allowed) {
        const std::string gate = context.gates.firstBlocking.has_value()
                                     ? optimizer::policy::GateIdToString(
                                           *context.gates.firstBlocking)
                                     : "unknown";
        ErrorLine{} << L"  --memory-clean --execute-self-trim refused [first blocked gate: "
                    << WidenAscii(gate) << L"] (no system call was made)\n";
        if (!context.auditWritable) {
            ErrorLine{} << L"  audit    : unavailable - a real action is refused before any"
                           L" system call (audit must be writable first)\n";
        }
        if (!context.inputs.compileTime) {
            ErrorLine{} << L"  hint     : this capability is compiled out by default"
                           L" (build option OPTIMIZER_ENABLE_MEMORY_CLEAN)\n";
        }
        if (!context.inputs.config) {
            ErrorLine{} << L"  hint     : set [memory].scheduled_clean_enabled = true and"
                           L" max_clean_level = \"light\" (or higher) in the config\n";
        }
        if (!context.inputs.commandLine) {
            ErrorLine{} << L"  hint     : pass --acknowledge-system-wide-side-effects"
                           L" (action-specific confirmation; --force is not accepted)\n";
        }
        return 2;
    }
    // 真实执行路径固定只做**最轻一档**（工作集修剪 = light）；更重的步骤属 S3/S4（R3）。
    const auto plan = optimizer::memory::PlanMemoryClean(
        optimizer::config::CleanLevel::Light, configuredMax, true);
    if (!plan.allowed || plan.steps.empty()) {
        ErrorLine{} << L"  --memory-clean --execute-self-trim refused: no executable step"
                       L" for this configuration (no system call was made)\n";
        return 2;
    }
    JournalAction("memory.clean_execute", target, true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: trim own process working set (single call, no privilege)");
    const auto before = optimizer::memory::QueryOwnWorkingSet();
    const std::optional<optimizer::memory::WorkingSetReading> beforeReading =
        before ? std::optional<optimizer::memory::WorkingSetReading>(before.Value())
               : std::nullopt;
    optimizer::memory::SelfWorkingSetTrimBackend backend;
    const auto executed = optimizer::memory::ExecuteMemoryCleanPlan(plan, backend);
    const auto after = optimizer::memory::QueryOwnWorkingSet();
    const std::optional<optimizer::memory::WorkingSetReading> afterReading =
        after ? std::optional<optimizer::memory::WorkingSetReading>(after.Value())
              : std::nullopt;
    const auto delta = optimizer::memory::MakeWorkingSetDelta(beforeReading, afterReading);
    const std::string deltaText = optimizer::memory::DescribeWorkingSetDelta(delta);

    bool ok = false;
    std::wstring detail;
    if (executed) {
        const auto& report = executed.Value();
        ok = report.ok;
        if (ok) {
            detail = L"self working set trim executed (";
            detail += std::to_wstring(report.succeeded);
            detail += L" step(s), own process only)";
        } else {
            detail = L"failed at step ";
            detail += WidenAscii(
                optimizer::memory::CleanKindToString(report.failedStep));
            detail += L": ";
            detail += report.failureDetail;
        }
    } else {
        const auto& error = executed.ErrorValue();
        detail = L"not executed [";
        detail += optimizer::common::ToString(error.domain);
        detail += L":";
        detail += std::to_wstring(error.code);
        detail += L"] ";
        detail += error.message;
    }
    std::string detailAscii;
    for (const wchar_t ch : detail) {
        detailAscii.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : 0x3F);
    }

    optimizer::common::WriteConsoleLine(
        L"Memory clean execute (R1; own process only, single call)");
    optimizer::common::WriteConsoleLine(
        L"  step     : working_set_trim (level light; no other step is executed)");
    {
        std::wostringstream line;
        line << L"  reading  : " << WidenAscii(deltaText);
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  verdict  : " << (ok ? L"executed" : L"failed");
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (!ok) {
        ErrorLine{} << L"  detail   : " << detail << L"\n";
    }
    optimizer::common::WriteConsoleLine(
        L"  note     : trimming a working set does not free system cache; the pages are"
        L" refilled on demand (reversible, R1)");
    AuditAction("memory.clean_execute", optimizer::audit::RiskLevel::R1, target,
                ok, detailAscii, deltaText);
    if (!ok) {
        return 2;
    }
    // 成功才写冷却（每用户台账；原子替换）。写失败如实上报，但不把已完成的动作说成失败。
    const auto recorded = optimizer::policy::RecordCooldownRun(
        DefaultCooldownLedgerPath(), optimizer::policy::kMemoryCleanCapabilityId,
        static_cast<std::int64_t>(std::time(nullptr)));
    if (!recorded) {
        const auto& error = recorded.ErrorValue();
        std::wostringstream line;
        line << L"  cooldown : not recorded ["
             << optimizer::common::ToString(error.domain) << L":" << error.code
             << L"] " << error.message
             << L" (the action itself did complete)";
        optimizer::common::WriteConsoleLine(line.str());
    }
    return 0;
}

// --memory-clean [config.toml] [--dry-run]：内存清理能力的**计划 / dry-run**；
// --execute-self-trim 为其**真实执行路径**（R1：只修剪本进程工作集）。
// 门禁未全通过时如实拒绝执行；dry-run 展示“若门禁开放将要执行什么”，并写审计 + 三段日记。
int RunMemoryCleanCommand(int argc, wchar_t* argv[]) {
    std::optional<std::filesystem::path> configPath;
    bool acknowledged = false;
    bool executeSelfTrim = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--dry-run") {
            continue; // 与“不给执行开关”同义（保留参数供脚本显式化）
        }
        if (arg == L"--acknowledge-system-wide-side-effects") {
            acknowledged = true; // 动作特定确认（不使用通用 --force）
            continue;
        }
        if (arg == L"--execute-self-trim") {
            executeSelfTrim = true; // R1：只修剪本进程工作集
            continue;
        }
        if (arg == L"--force") {
            ErrorLine{} << L"  --force is not accepted; use an action-specific"
                           L" confirmation such as --acknowledge-system-wide-side-effects\n";
            return 2;
        }
        if (arg == L"--execute") {
            ErrorLine{} << L"  --memory-clean --execute is refused: the heavier steps"
                           L" (standby list purge, system file cache trim) have no real"
                           L" backend yet and refuse with Unsupported. Use"
                           L" --execute-self-trim for the R1 self trim.\n";
            return 2;
        }
        if (configPath.has_value()) {
            ErrorLine{} << L"  unknown --memory-clean option: " << arg << L"\n";
            return 2;
        }
        configPath = std::filesystem::path(argv[i]);
    }
    optimizer::config::CleanLevel configuredMax = optimizer::config::CleanLevel::None;
    if (configPath.has_value()) {
        const auto loaded = optimizer::config::LoadConfig(configPath->wstring());
        if (!loaded) {
            const auto& error = loaded.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        configuredMax = loaded.Value().memory.maxCleanLevel;
    }
    if (executeSelfTrim) {
        return RunMemoryCleanSelfTrim(configPath, configuredMax, acknowledged);
    }
    // dry-run：门禁输入与真实执行路径同源（`GatherMemoryCleanGates`）。
    const auto context = GatherMemoryCleanGates(
        acknowledged, configuredMax != optimizer::config::CleanLevel::None);
    const auto gates = context.gates;
    const auto plan = optimizer::memory::PlanMemoryClean(configuredMax, configuredMax,
                                                         gates.allowed);
    const std::string target = configPath.has_value() ? "config" : "defaults";
    const wchar_t* levelName = L"none";
    switch (configuredMax) {
        case optimizer::config::CleanLevel::None:
            levelName = L"none";
            break;
        case optimizer::config::CleanLevel::Light:
            levelName = L"light";
            break;
        case optimizer::config::CleanLevel::Medium:
            levelName = L"medium";
            break;
        case optimizer::config::CleanLevel::Deep:
            levelName = L"deep";
            break;
    }
    JournalAction("memory.clean_plan", target, true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: plan memory clean (dry-run, no system call)");
    optimizer::common::WriteConsoleLine(
        L"Memory clean plan (dry-run; this mode performs no clean)");
    {
        std::wostringstream line;
        line << L"  level    : " << levelName << L" (from "
             << (configPath.has_value() ? configPath->wstring()
                                        : std::wstring(L"built-in default"))
             << L")";
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  plan     : ";
        if (plan.steps.empty()) {
            line << L"nothing to do (level is none)";
        } else {
            for (std::size_t i = 0; i < plan.steps.size(); ++i) {
                const std::string step =
                    optimizer::memory::CleanKindToString(plan.steps[i]);
                if (i > 0) {
                    line << L", ";
                }
                line << std::wstring(step.begin(), step.end());
            }
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  verdict  : "
             << (plan.allowed
                     ? L"gates passed (dry-run only: --execute-self-trim runs the R1 step)"
                     : L"refused (execution not performed)");
        if (!gates.allowed && gates.firstBlocking.has_value()) {
            const std::string gate =
                optimizer::policy::GateIdToString(*gates.firstBlocking);
            line << L" [first blocked gate: "
                 << std::wstring(gate.begin(), gate.end()) << L"]";
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    optimizer::common::WriteConsoleLine(
        L"  note     : no system call was made; --execute-self-trim runs the R1 self trim"
        L" (own process); the R3 steps have no real backend");
    {
        std::wostringstream line;
        line << L"  confirm  : acknowledge-system-wide-side-effects="
             << (acknowledged ? L"yes" : L"no")
             << L" (action-specific confirmation; --force is not accepted)";
        optimizer::common::WriteConsoleLine(line.str());
    }
    AuditAction("memory.clean_plan", optimizer::audit::RiskLevel::R2, target, true,
                "plan only (dry-run; no system call)",
                plan.steps.empty()
                    ? "no steps planned"
                    : "working_set_trim planned, dry-run (no execution)");
    return 0;
}

int RunGatesCommand(int argc, wchar_t* argv[]) {
    std::optional<optimizer::config::ConfigSnapshot> snapshot;
    bool acknowledged = false;
    bool jsonOut = false; // --json：机器可读输出（只读）
    for (int i = 2; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--acknowledge-system-wide-side-effects") {
            acknowledged = true;
            continue;
        }
        if (arg == L"--json") {
            jsonOut = true;
            continue;
        }
        if (arg == L"--force") {
            ErrorLine{} << L"  --force is not accepted; use an action-specific"
                           L" confirmation such as --acknowledge-system-wide-side-effects"
                        << L"\n";
            return 2;
        }
        if (snapshot.has_value()) {
            ErrorLine{} << L"  unknown --gates option: " << arg << L"\n";
            return 2;
        }
        {
            const auto loaded = optimizer::config::LoadConfig(arg);
            if (!loaded) {
                const auto& error = loaded.ErrorValue();
                ErrorLine{} << L"  config load failed ["
                            << optimizer::common::ToString(error.domain) << L":"
                            << error.code << L"] " << error.message << L"\n";
                return 2;
            }
            snapshot = loaded.Value();
        }
    }
    // 每个能力：只填**已知**的门；未实现的门一律显示 n/a 并计入裁决（不得假装已通过）。
    struct Capability {
        const wchar_t* name;
        bool configured;
    };
    const bool hasConfig = snapshot.has_value();
    const auto environment = GatherEnvironmentFacts();
    const bool environmentGateOpen =
        optimizer::policy::EvaluateEnvironmentGate(environment);
    // 审计门：只打开不写入的可写探测（不改变审计记录内容）。
    const bool auditGateOpen =
        static_cast<bool>(optimizer::audit::ProbeAuditWritable(DefaultAuditLogPath()));
    // 编译期开关（默认关）与冷却台账（每用户文件；默认 15 分钟）。
    const bool compiledIn = optimizer::policy::MemoryCleanCompiledIn();
    const auto cooldownLedger =
        optimizer::policy::ReadCooldownLedger(DefaultCooldownLedgerPath());
    const auto nowUnix = static_cast<std::int64_t>(std::time(nullptr));
    const bool cooldownGateOpen =
        cooldownLedger &&
        optimizer::policy::EvaluateCooldownGate(
            cooldownLedger.Value(), optimizer::policy::kMemoryCleanCapabilityId,
            nowUnix,
            std::chrono::duration_cast<std::chrono::seconds>(
                optimizer::policy::kDefaultCooldown));
    const Capability capabilities[] = {
        {L"power.switch_power_scheme",
         hasConfig && snapshot->power.switchPowerScheme},
        {L"memory.scheduled_clean_enabled",
         hasConfig && snapshot->memory.scheduledCleanEnabled},
        {L"memory.allow_native_write", hasConfig && snapshot->memory.allowNativeWrite},
        {L"memory.max_clean_level",
         hasConfig &&
             snapshot->memory.maxCleanLevel != optimizer::config::CleanLevel::None},
    };
    // --json：机器可读输出（只读）。放在事实采集之后、人读表头之前，避免先输出横幅污染 JSON。
    if (jsonOut) {
        optimizer::policy::GatesReport report;
        report.acknowledged = acknowledged;
        report.factsKnown = environment.factsKnown;
        report.osSupported = environment.osSupported;
        report.onBattery = environment.onBattery;
        report.remoteSession = environment.remoteSession;
        report.sessionLocked = environment.sessionLocked;
        report.auditWritable = auditGateOpen;
        for (const auto& capability : capabilities) {
            optimizer::policy::GateInputs inputs;
            inputs.compileTime = compiledIn;
            inputs.config = capability.configured;
            inputs.commandLine = acknowledged;
            inputs.permissionAndEnvironment = environmentGateOpen;
            inputs.audit = auditGateOpen;
            inputs.cooldown = cooldownGateOpen;
            const auto evaluation = optimizer::policy::EvaluateGates(inputs);
            optimizer::policy::GatesReportEntry entry;
            std::string name;
            for (const wchar_t ch : std::wstring(capability.name)) {
                name.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch)
                                                    : 0x3F);
            }
            entry.name = name;
            entry.allowed = evaluation.allowed;
            if (evaluation.firstBlocking.has_value()) {
                entry.firstBlocking =
                    optimizer::policy::GateIdToString(*evaluation.firstBlocking);
            }
            report.capabilities.push_back(std::move(entry));
        }
        const std::string json = optimizer::policy::FormatGatesJson(report);
        optimizer::common::WriteConsoleLine(std::wstring(json.begin(), json.end()));
        return 0;
    }
    optimizer::common::WriteConsoleLine(
        L"Safety gates (read-only; no action is performed)");
    optimizer::common::WriteConsoleLine(
        L"  capability                   compile config cmdline perm_env audit cooldown  verdict");
    for (const auto& capability : capabilities) {
        optimizer::policy::GateInputs inputs;
        inputs.compileTime = compiledIn;     // 编译期开关（默认关）
        inputs.config = capability.configured;
        inputs.commandLine = acknowledged;   // 动作特定确认（命令行显式动作）
        inputs.permissionAndEnvironment = environmentGateOpen; // 真实只读探测（电池/远程/锁屏…）
        inputs.audit = auditGateOpen;                        // 真实可写探测（只打开不写入）
        inputs.cooldown = cooldownGateOpen;                             // 未实现冷却门
        const auto evaluation = optimizer::policy::EvaluateGates(inputs);
        std::wostringstream line;
        line << L"  " << capability.name;
        const std::size_t nameWidth = wcslen(capability.name);
        for (std::size_t i = nameWidth; i < 31; ++i) { // 列宽 31（保证与后列至少一个空格）
            line << L' ';
        }
        // 列顺序：compile config cmdline perm_env audit cooldown（与表头一致）。
        const auto mark = [](bool value) -> const wchar_t* {
            return value ? L"yes" : L"no";
        };
        line << mark(evaluation.gates.compileTime) << L"     "
             << mark(evaluation.gates.config) << L"      "
             << mark(evaluation.gates.commandLine) << L"      "
             << mark(environmentGateOpen) << L"      "
             << mark(auditGateOpen) << L"     "
             << mark(evaluation.gates.cooldown) << L"        "
             << (evaluation.allowed ? L"allowed" : L"blocked");
        if (!evaluation.allowed && evaluation.firstBlocking.has_value()) {
            const std::string gate =
                optimizer::policy::GateIdToString(*evaluation.firstBlocking);
            line << L" (first: " << std::wstring(gate.begin(), gate.end()) << L")";
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    optimizer::common::WriteConsoleLine(
        L"  confirm  : acknowledge-system-wide-side-effects="
        + std::wstring(acknowledged ? L"yes" : L"no"));
    optimizer::common::WriteConsoleLine(
        L"  note     : R2/R3 real actions additionally require an isolated environment;"
        L" this command performs nothing");
    {
        // 环境事实如实展示（只读）：未知也如实说“未知”。
        std::wostringstream line;
        line << L"  env      : os="
             << (environment.factsKnown ? (environment.osSupported ? L"supported"
                                                                  : L"unsupported")
                                        : L"unknown")
             << L" power=" << (environment.factsKnown
                                   ? (environment.onBattery ? L"battery" : L"ac")
                                   : L"unknown")
             << L" session="
             << (environment.factsKnown
                     ? (environment.remoteSession
                            ? L"remote"
                            : (environment.sessionLocked ? L"locked" : L"interactive"))
                     : L"unknown")
             << L" audit=" << (auditGateOpen ? L"writable" : L"unavailable");
        optimizer::common::WriteConsoleLine(line.str());
    }
    return 0;
}

int RunConfigCommand(std::wstring_view path) {
    // --config <path>: 解析并校验 TOML 配置，输出关键项，只读。
    auto result = optimizer::config::LoadConfig(path);
    if (!result) {
        const auto& error = result.ErrorValue();
        ErrorLine{} << L"  config load failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    const auto& c = result.Value();
    const wchar_t* mode = L"observe";
    if (c.application.mode == optimizer::config::RunMode::Balanced) {
        mode = L"balanced";
    } else if (c.application.mode == optimizer::config::RunMode::Experimental) {
        mode = L"experimental";
    }
    std::wcout << L"Config snapshot (read-only)\n";
    const std::string versionText =
        optimizer::config::FormatConfigVersion(c.version);
    std::wcout << L"  version    : "
               << std::wstring(versionText.begin(), versionText.end())
               << L"\n";
    std::wcout << L"  mode       : " << mode << L"\n";
    std::wcout << L"  logging    : level " << std::wstring(c.logging.level.begin(),
                                                        c.logging.level.end())
               << L", max " << c.logging.maxFileMb << L" MB x "
               << c.logging.maxFiles << L" backup file(s)\n";
    std::wcout << L"  memory     : query " << (c.memory.queryEnabled ? L"on" : L"off")
               << L", clean " << (c.memory.scheduledCleanEnabled ? L"on" : L"off")
               << L", native-write " << (c.memory.allowNativeWrite ? L"on" : L"off")
               << L"\n";
    std::wcout << L"  layers     : monitor " << (c.layers.monitoring ? L"on" : L"off")
               << L", maintenance " << (c.layers.maintenance ? L"on" : L"off")
               << L", emergency " << (c.layers.emergency ? L"on" : L"off") << L"\n";
    std::wcout << L"  power      : switch-scheme "
               << (c.power.switchPowerScheme ? L"on" : L"off") << L"\n";
    std::wcout << L"  priority   : " << (c.priority.enabled ? L"enabled" : L"disabled")
               << L" (max level set)\n";
    std::wcout << L"  policy     : margins "
               << c.policy.comfortableMarginPercent << L"/"
               << c.policy.adequateMarginPercent << L"/"
               << c.policy.tightMarginPercent << L" cooldown "
               << c.policy.cooldownMs << L" ms";
    if (c.policy.userAwayIdleSeconds > 0) {
        std::wcout << L" user-away ";
        std::wcout << L"on (idle >= " << c.policy.userAwayIdleSeconds
                   << L" s)";
    } else {
        std::wcout << L" user-away off";
    }
    if (c.policy.haltAfterActionFailures > 0) {
        std::wcout << L" halt ";
        std::wcout << L"on (after " << c.policy.haltAfterActionFailures
                   << L" action failures)";
    } else {
        std::wcout << L" halt off";
    }
    std::wcout << L"\n";
    std::wcout << L"  games      : " << c.games.size() << L" rule(s)\n";
    std::wcout << L"  agent      : form "
               << std::wstring(c.agent.form.begin(), c.agent.form.end())
               << L" (declared preference; registration is always an explicit command)\n";
    // 如实区分“已解析”与“已生效”：以下字段尚无消费者（能力待落地或模块未实施），
    // 回显它们不代表行为已生效（与日志/审计同口径：不伪装成功）。
    std::wcout << L"  pending    : parsed but not effective yet:\n"
                  L"               [gpu_heartbeat]/[scheduler]/[disk_cache] (modules not implemented)\n";
    // 未知键如实上报（拼写错误不再被静默吞掉）。键名允许非 ASCII，故先冲刷 std::wcout
    // 缓冲再走双路径输出（std::wcout 在默认 C locale 下遇非 ASCII 会进入 failbit 截断后续输出）。
    if (!c.unknownKeys.empty()) {
        std::wcout.flush();
        std::wostringstream unknownHead;
        unknownHead << L"  unknown    : " << c.unknownKeys.size()
                    << L" ignored key(s) (typo? defaults were used):";
        optimizer::common::WriteConsoleLine(unknownHead.str());
        for (const auto& key : c.unknownKeys) {
            const auto wide = optimizer::common::Utf8ToWide(key);
            optimizer::common::WriteConsoleLine(
                std::wstring(L"               ") +
                (wide ? wide.Value() : L"<undecodable key>"));
        }
    }
    return 0;
}

int RunActivityCommand(int argc, wchar_t* argv[]) {
    // --activity <s> [idle-secs] [--session] [--foreground]：用户输入活动观测（MOD-ACT-001，
    // R0 只读、前台有界）。每秒只读查询最近键鼠输入（GetLastInputInfo）并分类
    // Active/Idle/Unknown；--session（ACT-002）追加当前会话上下文：远程会话标志
    // （SM_REMOTESESSION）＋锁屏（WTSSessionInfoEx SessionFlags）＋会话连接状态
    // （WTSConnectState），断开会话/锁屏覆盖输入态（Disconnected/Locked），会话或输入任一查询
    // 失败按 Unknown 降级不伪装；--foreground（ACT-003）追加前台窗口归属：仅 Active/Idle 样本
    // 查询一次前台窗口所属进程（GetForegroundWindow + GetWindowThreadProcessId）并输出
    // fg=<pid>，无窗口输出 fg=none、查询失败 fg=unavailable；Locked/Disconnected/Unknown 不可
    // 归属（不伪造 pid）。可与 --session 同时使用（顺序无关）。无 Hook、无窗口/消息循环/后台
    // 线程、不采集输入内容与窗口标题。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (argc < 3 || !ParseUint32(argv[2], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --activity seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }
    std::uint32_t idleSecs = 15; // 默认空闲阈值 15 秒
    bool idleGiven = false;
    bool sessionContext = false;
    bool foreground = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--session") {
            sessionContext = true;
            continue;
        }
        if (arg == L"--foreground") {
            foreground = true;
            continue;
        }
        if (arg.size() >= 2 && arg[0] == L'-') {
            ErrorLine{} << L"  --activity unknown option: " << arg << L"\n";
            return 2;
        }
        if (idleGiven || !ParseUint32(arg, idleSecs) || idleSecs == 0 ||
            idleSecs > 3600) {
            ErrorLine{} << L"  --activity idle-secs must be in 1..3600\n";
            return 2;
        }
        idleGiven = true; // 位置参数仅一个（idle-secs），顺序与 --session 无关
    }

    std::wcout << L"User activity observation (read-only, foreground, "
               << seconds << L" s)\n";
    std::wcout << L"  threshold : idle >= " << idleSecs << L" s\n";
    std::wcout << L"  source    : GetLastInputInfo (no hooks, no input content)";
    auto backend = optimizer::activity::CreateWin32LastInputBackend();
    auto sessionProbe = optimizer::activity::CreateWin32SessionProbe();
    if (sessionContext) {
        const auto context = sessionProbe->Query();
        if (!context) {
            std::wcout
                << L"\n  context   : session probe unavailable - per-sample "
                   L"degraded to unknown";
        } else {
            std::wcout << L"\n  context   : "
                       << (context.Value().remoteSession
                               ? L"remote (RDP) session"
                               : L"local session")
                       << L"; states may include locked/disconnected";
        }
    }
    if (foreground) {
        std::wcout
            << L"\n  fg        : GetForegroundWindow + "
               L"GetWindowThreadProcessId (window owning pid, read-only; "
               L"active/idle samples only)";
    }
    std::wcout << L"\n";

    if (foreground) {
        auto foregroundProbe =
            optimizer::activity::CreateWin32ForegroundProbe();
        std::size_t sampleIndex = 0;
        const auto onSample =
            [&seconds, &sampleIndex](
                optimizer::activity::ActivityState state, std::int64_t idleMs,
                const optimizer::activity::ForegroundAttributionInfo& info) {
                ++sampleIndex;
                std::wcout << L"  [" << sampleIndex << L"/" << seconds << L"] "
                           << optimizer::activity::ActivityStateToString(state);
                // 锁屏/断开/未知不带 idle（active/idle 才打印输入空闲毫秒）。
                if (state == optimizer::activity::ActivityState::Active ||
                    state == optimizer::activity::ActivityState::Idle) {
                    std::wcout << L" (idle " << idleMs << L" ms)";
                }
                switch (info.attribution) {
                    case optimizer::activity::ForegroundAttribution::Pid:
                        std::wcout << L" fg=" << info.pid;
                        break;
                    case optimizer::activity::ForegroundAttribution::NoWindow:
                        std::wcout << L" fg=none";
                        break;
                    case optimizer::activity::ForegroundAttribution::Unknown:
                        std::wcout << L" fg=unavailable";
                        break;
                    default:
                        // NotApplicable（Locked/Disconnected/Unknown）：不伪造前台。
                        break;
                }
                std::wcout << L"\n";
            };
        optimizer::activity::SessionProbe* session = nullptr;
        if (sessionContext) {
            session = sessionProbe.get();
        }
        const auto result = optimizer::activity::ObserveActivityForeground(
            *backend, session, *foregroundProbe, seconds,
            std::chrono::milliseconds(1000),
            static_cast<std::int64_t>(idleSecs) * 1000, onSample);
        if (!result) {
            const auto& error = result.ErrorValue();
            ErrorLine{} << L"  observation failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        const auto& summary = result.Value();
        if (sessionContext) {
            std::wcout << L"  summary  : active " << summary.states.active
                       << L" / idle " << summary.states.idle << L" / locked "
                       << summary.states.locked << L" / disconnected "
                       << summary.states.disconnected << L" / unknown "
                       << summary.states.unknown << L"\n";
        } else {
            std::wcout << L"  summary  : active " << summary.states.active
                       << L" / idle " << summary.states.idle << L" / unknown "
                       << summary.states.unknown << L"\n";
        }
        std::wcout << L"  fg        : pid " << summary.attributedPid
                   << L" / none " << summary.noWindow << L" / unavailable "
                   << summary.foregroundUnknown << L" / n/a "
                   << summary.notApplicable << L"\n";
        if (summary.states.unknown == summary.states.samples &&
            summary.states.samples > 0) {
            std::wcout
                << L"  -> "
                << (sessionContext
                        ? L"input/session query unavailable; degraded to unknown, "
                          L"not Active/Idle/Locked/Disconnected"
                        : L"last-input query unavailable (non-interactive "
                          L"session?); degraded to unknown, not Active/Idle")
                << L"\n";
        } else if (summary.foregroundUnknown > 0 &&
                   summary.foregroundUnknown ==
                       summary.states.active + summary.states.idle) {
            std::wcout
                << L"  -> foreground query unavailable on active/idle samples; "
                   L"pid not fabricated\n";
        }
        return 0;
    }
    std::size_t sampleIndex = 0;
    const auto onSample =
        [&seconds, &sampleIndex](optimizer::activity::ActivityState state,
                                 std::int64_t idleMs) {
            ++sampleIndex;
            std::wcout << L"  [" << sampleIndex << L"/" << seconds << L"] "
                       << optimizer::activity::ActivityStateToString(state);
            // 锁屏/断开/未知不带 idle（active/idle 才打印输入空闲毫秒）。
            if (state == optimizer::activity::ActivityState::Active ||
                state == optimizer::activity::ActivityState::Idle) {
                std::wcout << L" (idle " << idleMs << L" ms)";
            }
            std::wcout << L"\n";
        };
    if (sessionContext) {
        const auto result = optimizer::activity::ObserveActivityContext(
            *backend, *sessionProbe, seconds, std::chrono::milliseconds(1000),
            static_cast<std::int64_t>(idleSecs) * 1000, onSample);
        if (!result) {
            const auto& error = result.ErrorValue();
            ErrorLine{} << L"  observation failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        const auto& summary = result.Value();
        std::wcout << L"  summary  : active " << summary.active << L" / idle "
                   << summary.idle << L" / locked " << summary.locked
                   << L" / disconnected " << summary.disconnected
                   << L" / unknown " << summary.unknown << L"\n";
        if (summary.unknown == summary.samples && summary.samples > 0) {
            std::wcout << L"  -> input/session query unavailable; degraded to "
                          L"unknown, not Active/Idle/Locked/Disconnected\n";
        }
        return 0;
    }

    const auto result = optimizer::activity::ObserveActivity(
        *backend, seconds, std::chrono::milliseconds(1000),
        static_cast<std::int64_t>(idleSecs) * 1000, onSample);
    if (!result) {
        const auto& error = result.ErrorValue();
        ErrorLine{} << L"  observation failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    const auto& summary = result.Value();
    std::wcout << L"  summary  : active " << summary.active << L" / idle "
               << summary.idle << L" / unknown " << summary.unknown << L"\n";
    if (summary.unknown == summary.samples && summary.samples > 0) {
        std::wcout
            << L"  -> last-input query unavailable (non-interactive session?);\n"
               L"     degraded to unknown, not Active/Idle\n";
    }
    return 0;
}

int RunCpuCommand() {
    // --cpu: 前台、有界、只读的两次 PDH 采样，首次为 warming-up 基线。
    // 无后台线程、无周期任务。
    optimizer::metrics::PdhCpuQuery query;
    auto init = query.Initialize();
    if (!init) {
        ErrorLine{} << L"  cpu query init failed ["
                   << optimizer::common::ToString(init.ErrorValue().domain) << L":"
                   << init.ErrorValue().code << L"] "
                   << init.ErrorValue().message << L"\n";
        return 2;
    }

    auto first = query.Sample(); // warming-up 基线
    if (!first) {
        ErrorLine{} << L"  cpu sample failed ["
                   << optimizer::common::ToString(first.ErrorValue().domain)
                   << L":" << first.ErrorValue().code << L"] "
                   << first.ErrorValue().message << L"\n";
        return 2;
    }

    auto second = query.Sample();
    if (!second || !second.Value().valid) {
        ErrorLine{} << L"  cpu sample not ready (warming up)\n";
        return 2;
    }

    std::wcout << L"CPU usage (read-only, PDH, two samples)\n";
    std::wcout << L"  % Processor Time : " << second.Value().usagePercent
               << L"%\n";
    return 0;
}

int RunLogCommand(int argc, wchar_t* argv[]) {
    // --log <module> <message...>: 向 stderr 写入一条同步 Info 记录。
    // 只读、前台、无后台线程；消息文本永不被当作命令解析。
    if (argc < 4) {
        ErrorLine{} << L"  --log requires a module and a message\n";
        return 2;
    }
    std::wstring message;
    for (int i = 3; i < argc; ++i) {
        if (i > 3) {
            message.push_back(L' ');
        }
        message.append(argv[i]);
    }
    optimizer::logger::Logger logger;
    logger.SetStderrSink();
    logger.Write(optimizer::logger::LogLevel::Info, argv[2], message);
    return 0;
}

int RunAuditSummaryCommand(int argc, wchar_t* argv[]) {
    // --audit-summary [path] [lines]：只读回看已压缩汇总（默认每用户 audit.log 对应的汇总文件，
    // 最近 20 行、上限 200）。不写不删；“尚无已压缩汇总”与“读取失败”分开报（后者如实失败）。
    // 路径参数指“审计文件”：汇总路径由同一命名规则推导（AuditSummaryPath），不重复实现命名。
    constexpr std::size_t kDefaultLines = 20;
    constexpr std::uint32_t kMaxLines = 200;
    const std::filesystem::path auditPath =
        argc >= 3 ? std::filesystem::path(argv[2]) : DefaultAuditLogPath();
    const std::filesystem::path path =
        optimizer::audit::AuditSummaryPath(auditPath);
    std::size_t maxLines = kDefaultLines;
    if (argc >= 4) {
        std::uint32_t parsed = 0;
        if (!ParseUint32(argv[3], parsed) || parsed < 1 || parsed > kMaxLines) {
            optimizer::common::WriteConsoleLine(
                L"  --audit-summary <lines> must be 1..200");
            return 2;
        }
        maxLines = parsed;
    }
    const auto tail = optimizer::audit::ReadAuditTail(path, maxLines);
    if (!tail) {
        const auto& error = tail.ErrorValue();
        ErrorLine{} << L"  audit summary read failed ["
                    << optimizer::common::ToString(error.domain) << L":"
                    << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    optimizer::common::WriteConsoleLine(L"Audit compaction summary (read-only)");
    {
        std::wostringstream line;
        line << L"  path    : " << path.wstring();
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (tail.Value().totalLines == 0) {
        optimizer::common::WriteConsoleLine(
            L"  note    : no compacted summaries yet (nothing was compacted)");
        return 0;
    }
    // 先给聚合结论（跨多行汇总），再逐行展示；不可解析行计入 unparsable 并原样输出。
    const auto totals =
        optimizer::audit::AnalyzeAuditSummaryLines(tail.Value().lines);
    {
        std::wostringstream line;
        line << L"  compacted: " << totals.total << L" line(s) in "
             << totals.ranges << L" range(s); ok " << totals.ok << L", fail "
             << totals.fail << L", unparsed " << totals.unparsed;
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (!totals.firstTimestamp.empty()) {
        // 汇总行时间戳为 ASCII；仍走 UTF-8 解码路径以与其它回看输出一致（非法字节不伪装）。
        const auto first = optimizer::common::Utf8ToWide(totals.firstTimestamp);
        const auto last = optimizer::common::Utf8ToWide(totals.lastTimestamp);
        std::wostringstream line;
        line << L"  range   : " << (first ? first.Value() : L"<undecodable>")
             << L" .. " << (last ? last.Value() : L"<undecodable>");
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (totals.unparsable > 0) {
        std::wostringstream line;
        line << L"  unreadable: " << totals.unparsable
             << L" line(s) not counted (kept verbatim below)";
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  lines   : showing last " << tail.Value().lines.size()
             << L" of " << tail.Value().totalLines;
        optimizer::common::WriteConsoleLine(line.str());
    }
    for (const auto& line : tail.Value().lines) {
        const auto wide = optimizer::common::Utf8ToWide(line);
        optimizer::common::WriteConsoleLine(
            wide ? wide.Value() : L"  <undecodable audit summary line>");
    }
    return 0;
}

int RunAuditCompactCommand(int argc, wchar_t* argv[]) {
    // --audit-compact [path]：手动触发审计语义压缩（R0，仅本地文件）。
    // 两阶段（压缩优先、永不删除）：
    //   1) 正常审计文件达触发阈值（默认 5 MiB）-> 旧记录聚合为一行汇总写入同目录汇总文件；
    //   2) 汇总文件达上限（默认 20 MiB）-> 把多行汇总折叠为一行合并汇总（计数与时间范围保留）。
    // 不可解析行在两阶段都原样保留；两阶段均以临时文件 + 原子替换重写，失败不破坏原文件。
    const std::filesystem::path path = argc >= 3 ? std::filesystem::path(argv[2])
                                                : DefaultAuditLogPath();
    const optimizer::audit::CompactOptions options{};
    const auto compacted = optimizer::audit::CompactAuditFile(path, options);
    if (!compacted) {
        const auto& error = compacted.ErrorValue();
        ErrorLine{} << L"  audit compact failed ["
                    << optimizer::common::ToString(error.domain) << L":"
                    << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    const auto& result = compacted.Value();
    {
        std::wostringstream line;
        line << L"  path    : " << path.wstring();
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (!result.compacted) {
        std::wostringstream line;
        line << L"  result  : skipped (below threshold or nothing to compact; "
             << result.beforeLines << L" line(s) untouched)";
        optimizer::common::WriteConsoleLine(line.str());
    } else {
        std::wostringstream line;
        line << L"  result  : compacted " << result.beforeLines << L" -> "
             << result.afterLines << L" line(s); summarized "
             << result.summarizedLines << L", unparsed kept "
             << result.unparsedKept;
        optimizer::common::WriteConsoleLine(line.str());
        {
            std::wostringstream summaryLine;
            summaryLine << L"  summary : " << result.summaryPath.wstring();
            optimizer::common::WriteConsoleLine(summaryLine.str());
        }
    }
    // 汇总文件上限独立检查：即使审计文件未达阈值，汇总文件达上限也要折叠。
    const auto folded = optimizer::audit::FoldAuditSummaryFile(
        optimizer::audit::AuditSummaryPath(path), options);
    if (!folded) {
        const auto& error = folded.ErrorValue();
        ErrorLine{} << L"  audit summary fold failed ["
                    << optimizer::common::ToString(error.domain) << L":"
                    << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    if (folded.Value().folded) {
        std::wostringstream line;
        line << L"  folded  : " << folded.Value().mergedRanges
             << L" summarized range(s) -> 1 line (" << folded.Value().beforeLines
             << L" -> " << folded.Value().afterLines << L" line(s); unparsable kept "
             << folded.Value().unparsableKept << L")";
        optimizer::common::WriteConsoleLine(line.str());
    }
    return 0;
}

// --journal [path] [lines]：只读回看**动作日记**（默认每用户 action-journal.log，最近 20 行，上限 200）。
// 与 --audit-log 同口径：纯数字首参按行数解读；不写、不截断、不删；"尚无记录"与"读取失败"分开报。
int RunJournalCommand(int argc, wchar_t* argv[]) {
    constexpr std::size_t kDefaultLines = 20;
    constexpr std::uint32_t kMaxLines = 200;
    std::filesystem::path path = DefaultJournalPath();
    std::size_t maxLines = kDefaultLines;
    std::uint32_t leadingLines = 0;
    const bool leadingIsLines =
        argc >= 3 && ParseUint32(argv[2], leadingLines) && leadingLines >= 1 &&
        leadingLines <= kMaxLines;
    if (argc >= 3 && !leadingIsLines) {
        path = argv[2];
    }
    if (leadingIsLines) {
        maxLines = leadingLines;
    }
    if (argc >= 4) {
        std::uint32_t parsed = 0;
        if (!ParseUint32(argv[3], parsed) || parsed < 1 || parsed > kMaxLines) {
            optimizer::common::WriteConsoleLine(
                L"  --journal <lines> must be 1..200");
            return 2;
        }
        maxLines = parsed;
    }
    const auto tail = optimizer::audit::ReadAuditTail(path, maxLines);
    if (!tail) {
        const auto& error = tail.ErrorValue();
        std::wostringstream line;
        line << L"  journal read failed ["
             << optimizer::common::ToString(error.domain) << L":"
             << error.code << L"] " << error.message;
        optimizer::common::WriteConsoleLine(line.str());
        return 2;
    }
    optimizer::common::WriteConsoleLine(L"Action journal (read-only)");
    {
        std::wostringstream line;
        line << L"  path    : " << path.wstring();
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  records : " << tail.Value().totalLines << L" line(s)";
        if (tail.Value().totalLines > 0) {
            line << L", showing last " << tail.Value().lines.size();
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (tail.Value().totalLines == 0) {
        optimizer::common::WriteConsoleLine(
            L"  note    : no journal entries yet (dangerous/registration actions write");
        optimizer::common::WriteConsoleLine(
            L"            before/after/state entries when they run)");
        return 0;
    }
    std::size_t before = 0;
    std::size_t after = 0;
    std::size_t state = 0;
    std::size_t unparsable = 0;
    for (const auto& line : tail.Value().lines) {
        const auto entry = optimizer::audit::ParseJournalLine(line);
        if (!entry) {
            ++unparsable;
            continue;
        }
        if (entry->phase == "before") {
            ++before;
        } else if (entry->phase == "after") {
            ++after;
        } else if (entry->phase == "state") {
            ++state;
        } else {
            ++unparsable;
        }
    }
    {
        std::wostringstream line;
        line << L"  phases  : before " << before << L", after " << after
             << L", state " << state;
        if (unparsable > 0) {
            line << L", unparsable " << unparsable;
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    optimizer::common::WriteConsoleLine(
        L"  note    : append-only; local only (never aggregated by --audit-compact)");
    for (const auto& line : tail.Value().lines) {
        const auto wide = optimizer::common::Utf8ToWide(line);
        optimizer::common::WriteConsoleLine(
            wide ? wide.Value() : L"  <undecodable journal line>");
    }
    return 0;
}

int RunAuditLogCommand(int argc, wchar_t* argv[]) {
    // --audit-log [path] [lines]：只读回看审计文件末尾（默认每用户 audit.log，最近 20 行）。
    // 只读：不写、不截断、不重命名、不删除；“尚无记录”与“读取失败”分开报（后者如实失败）。
    // 输出统一走 WriteConsoleLine（双路径编码）：与直写句柄混用会打乱顺序，而 std::wcout
    // 在默认 C locale 下遇中文进入 failbit 会截断错误消息。
    constexpr std::size_t kDefaultLines = 20;
    constexpr std::uint32_t kMaxLines = 200;
    std::filesystem::path path = DefaultAuditLogPath();
    std::size_t maxLines = kDefaultLines;
    // 参数 UX：`--audit-log [path] [lines]` 中，纯数字的第一个参数按**行数**解读
    // （用户写 `--audit-log 6` 的本意几乎总是“最近 6 行”，把它当路径会得到空结果）。
    std::uint32_t leadingLines = 0;
    const bool leadingIsLines =
        argc >= 3 && ParseUint32(argv[2], leadingLines) && leadingLines >= 1 &&
        leadingLines <= kMaxLines;
    if (argc >= 3 && !leadingIsLines) {
        path = argv[2];
    }
    if (leadingIsLines) {
        maxLines = leadingLines;
    }
    if (argc >= 4) {
        std::uint32_t parsed = 0;
        if (!ParseUint32(argv[3], parsed) || parsed < 1 || parsed > kMaxLines) {
            optimizer::common::WriteConsoleLine(
                L"  --audit-log <lines> must be 1..200");
            return 2;
        }
        maxLines = parsed;
    }
    const auto tail = optimizer::audit::ReadAuditTail(path, maxLines);
    if (!tail) {
        const auto& error = tail.ErrorValue();
        std::wostringstream line;
        line << L"  audit log read failed ["
             << optimizer::common::ToString(error.domain) << L":"
             << error.code << L"] " << error.message;
        optimizer::common::WriteConsoleLine(line.str());
        return 2;
    }
    optimizer::common::WriteConsoleLine(L"Audit trail (read-only)");
    {
        std::wostringstream line;
        line << L"  path    : " << path.wstring();
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  records : " << tail.Value().totalLines << L" line(s)";
        if (tail.Value().totalLines > 0) {
            line << L", showing last " << tail.Value().lines.size();
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (tail.Value().totalLines == 0) {
        optimizer::common::WriteConsoleLine(
            L"  note    : no audit records yet; R1 actions are recorded when ready");
        optimizer::common::WriteConsoleLine(
            L"            config gates enable [priority].enabled or");
        optimizer::common::WriteConsoleLine(
            L"            [power].execution_required");
        return 0;
    }
    optimizer::common::WriteConsoleLine(
        L"  note    : append-only; no rotation or retention configured yet");
    // 已压缩部分的如实披露（AUD-004）：汇总文件不存在则不输出，避免无罪噪声。
    {
        const auto summaryPath = optimizer::audit::AuditSummaryPath(path);
        std::error_code existsEc;
        if (std::filesystem::exists(summaryPath, existsEc)) {
            const auto summaryTail =
                optimizer::audit::ReadAuditTail(summaryPath, kMaxLines);
            if (!summaryTail) {
                const auto& error = summaryTail.ErrorValue();
                std::wostringstream line;
                line << L"  compacted : summary read failed ["
                     << optimizer::common::ToString(error.domain) << L":"
                     << error.code << L"] " << error.message;
                optimizer::common::WriteConsoleLine(line.str());
            } else {
                const auto totals = optimizer::audit::AnalyzeAuditSummaryLines(
                    summaryTail.Value().lines);
                if (totals.ranges > 0) {
                    std::wostringstream line;
                    line << L"  compacted : " << totals.total << L" line(s) in "
                         << totals.ranges << L" summarized range(s) -> "
                         << summaryPath.wstring();
                    optimizer::common::WriteConsoleLine(line.str());
                } else if (totals.unparsable > 0) {
                    std::wostringstream line;
                    line << L"  compacted : " << totals.unparsable
                         << L" unreadable summary line(s) in "
                         << summaryPath.wstring() << L" (counts unavailable)";
                    optimizer::common::WriteConsoleLine(line.str());
                }
            }
        }
    }
    for (const auto& line : tail.Value().lines) {
        // 审计文件为 UTF-8：按 UTF-8 解码后走双路径输出；非法字节如实标注不伪装。
        const auto wide = optimizer::common::Utf8ToWide(line);
        optimizer::common::WriteConsoleLine(
            wide ? wide.Value() : L"  <undecodable audit line>");
    }
    return 0;
}

int RunWatchCommand(int argc, wchar_t* argv[]) {
    // --watch <seconds> [config-path]: 前台、有界、只读的进程生命周期观测。
    // 后台轮询线程仅在命令执行期间存在，命令结束后立即停止。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (!ParseUint32(argv[2], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --watch seconds must be in 1.." << kMaxSeconds << L"\n";
        return 2;
    }

    std::vector<optimizer::config::GameConfig> games;
    if (argc >= 4) {
        // main + 同目录 config.local.toml 合并（用户自建规则生效）。
        const std::filesystem::path mainPath(argv[3]);
        const std::wstring localPath =
            (mainPath.parent_path() / L"config.local.toml").wstring();
        auto config =
            optimizer::config::LoadConfigWithLocal(argv[3], localPath);
        if (!config) {
            const auto& error = config.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        games = std::move(config.Value().games);
    } else {
        // 无 main：仅加载当前目录 config.local.toml（存在时）。
        std::error_code existsError;
        if (std::filesystem::exists(
                std::filesystem::path(L"config.local.toml"), existsError)) {
            auto local = optimizer::config::LoadConfig(L"config.local.toml");
            if (local) {
                games = std::move(local.Value().games);
            }
        }
    }

    optimizer::process::ProcessWatcher watcher;
    auto setup = watcher.SetRules(games);
    if (!setup) {
        const auto& error = setup.ErrorValue();
        ErrorLine{} << L"  rule setup failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    std::wcout << L"Process watcher (read-only, foreground, " << seconds << L" s)\n";
    std::wcout << L"  rules : " << games.size() << L" game rule(s)\n";
    if (games.empty()) {
        std::wcout << L"  (pass --watch <s> <config.toml> to track game processes)\n";
    }

    watcher.Subscribe([](const optimizer::process::ProcessTransition& transition) {
        std::wostringstream line;
        line << L"  ["
             << optimizer::process::StateToString(transition.previous)
             << L" -> "
             << optimizer::process::StateToString(transition.current)
             << L"] "
             << std::wstring(transition.gameId.begin(), transition.gameId.end());
        if (transition.current == optimizer::process::ProcessState::Starting ||
            transition.current == optimizer::process::ProcessState::Running) {
            line << L" pid=" << transition.info.pid << L" "
                 << transition.info.processName
                 << (transition.info.isForeground ? L" foreground" : L"");
        }
        optimizer::common::WriteConsoleLine(line.str());
    });

    auto started = watcher.Start();
    if (!started) {
        const auto& error = started.ErrorValue();
        ErrorLine{} << L"  watcher start failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    watcher.Stop();

    const auto tracked = watcher.GetTrackedProcesses();
    std::wostringstream summary;
    summary << L"  tracked : " << tracked.size() << L" process(es)";
    optimizer::common::WriteConsoleLine(summary.str());
    for (const auto& info : tracked) {
        std::wostringstream line;
        line << L"    [" << std::wstring(info.gameId.begin(), info.gameId.end())
             << L"] " << info.processName << L" pid=" << info.pid << L" "
             << optimizer::process::StateToString(info.state);
        optimizer::common::WriteConsoleLine(line.str());
    }
    return 0;
}

int RunPolicyCommand(int argc, wchar_t* argv[]) {
    // --policy <s> [config.toml]：前台、有界策略决策窗口。
    // 每秒：内存余量（只读查询）-> 压力分级 -> 游戏焦点（单轮 Toolhelp 轮询）
    // -> 规则评估 + 防抖，输出决策；PWR-002 起，决策经配置门禁后落地为
    // R1 执行器动作（[priority].enabled -> 前台游戏提升；[power].execution_required
    // -> 游戏运行期持有电源请求），门禁全关时保持 POL-001 纯咨询行为。
    // ACT-004 起，[policy].user_away_idle_seconds > 0 时开启用户在场门禁：每秒只读
    // 查询最近输入（GetLastInputInfo），无输入达到阈值视用户不在场（AFK/锁屏/断开时
    // 输入时钟冻结自然落入），抑制优化建议（NoOp user_away）；查询失败同样视不在场
    //（在场未知不伪装在场——不优化是安全方向）。0（默认）关闭门禁，行为零回归。
    // 无效/缺失指标不触发决策（黄色不变量），但仍对账释放已持动作。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (!ParseUint32(argv[2], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --policy seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }

    // 配置加载：main + 同目录 config.local.toml 合并（与 --watch 一致）；
    // 无 main 时加载当前目录 config.local.toml（存在时）。
    std::vector<optimizer::config::GameConfig> games;
    optimizer::config::PolicyConfig policyConfig;
    optimizer::config::PowerConfig powerConfig;
    optimizer::config::PriorityConfig priorityConfig;
    optimizer::config::RunMode applicationMode =
        optimizer::config::RunMode::Observe; // [application].mode（默认最保守）
    optimizer::config::LayerConfig layersConfig; // [layers].*（默认仅 monitoring）
    bool configLoaded = false; // 无配置时执行门禁全关（保守默认）
    bool memoryQueryEnabled = true; // CFG-006：[memory].query_enabled（无配置 = 放行）
    if (argc >= 4) {
        const std::filesystem::path mainPath(argv[3]);
        const std::wstring localPath =
            (mainPath.parent_path() / L"config.local.toml").wstring();
        auto config =
            optimizer::config::LoadConfigWithLocal(argv[3], localPath);
        if (!config) {
            const auto& error = config.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        configLoaded = true;
        memoryQueryEnabled = config.Value().memory.queryEnabled; // CFG-006
        applicationMode = config.Value().application.mode;       // CFG-007
        layersConfig = config.Value().layers;                    // CFG-007
        games = config.Value().games;
        policyConfig = config.Value().policy;
        powerConfig = config.Value().power;
        priorityConfig = config.Value().priority;
    } else {
        std::error_code existsError;
        if (std::filesystem::exists(
                std::filesystem::path(L"config.local.toml"), existsError)) {
            auto local = optimizer::config::LoadConfig(L"config.local.toml");
            if (local) {
                configLoaded = true;
                games = std::move(local.Value().games);
                policyConfig = local.Value().policy;
                powerConfig = local.Value().power;
                priorityConfig = local.Value().priority;
            }
        }
    }

    // 后台暂停开关查询表（id -> pause_when_background）。
    std::map<std::string, bool> pauseByGame;
    for (const auto& game : games) {
        pauseByGame[game.id] = game.pauseWhenBackground;
    }

    optimizer::process::ProcessWatcher watcher;
    auto setup = watcher.SetRules(games);
    if (!setup) {
        const auto& error = setup.ErrorValue();
        ErrorLine{} << L"  rule setup failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const optimizer::policy::PolicyThresholds thresholds{
        policyConfig.comfortableMarginPercent,
        policyConfig.adequateMarginPercent,
        policyConfig.tightMarginPercent};
    optimizer::policy::PolicyEvaluator evaluator(
        thresholds, std::chrono::milliseconds(policyConfig.cooldownMs));

    // ACT-004：用户在场门禁。0 = 关闭（保守默认，零回归）；>0 秒无键鼠输入视用户不在场
    //（AFK/锁屏/断开时输入时钟冻结、空闲自然超阈值），抑制优化建议（NoOp user_away）。
    const bool userAwayGateOn = policyConfig.userAwayIdleSeconds > 0;
    const std::int64_t userAwayIdleMs =
        static_cast<std::int64_t>(policyConfig.userAwayIdleSeconds) * 1000;
    std::shared_ptr<optimizer::activity::LastInputBackend> lastInputBackend;
    if (userAwayGateOn) {
        lastInputBackend =
            optimizer::activity::CreateWin32LastInputBackend();
    }

    // PWR-002：决策经配置门禁后落地为 R1 执行器动作；
    // 无配置或门禁关闭 = 纯咨询（POL-001 行为不变）。
    optimizer::policy::ExecutorConfig executorConfig;
    // CFG-006：配置显式关闭内存查询时拒绝本命令（内存余量是策略决策的输入，不静默降级）。
    if (configLoaded && !memoryQueryEnabled) {
        ErrorLine{} << L"  memory query disabled by config ([memory].query_enabled = false)\n";
        return 2;
    }

    // CFG-007（2026-09-17）：[application].mode 与 [layers].* 纳入 R1 门禁（叠加关系，保守默认）。
    // 效果：默认 mode=observe 且 layers 默认 maintenance/emergency=false -> R1 动作全部被抑制；
    // 要启用 R1 需显式 mode=balanced/experimental **且** layers.maintenance（或 emergency）= true。
    const bool r1Allowed =
        configLoaded && optimizer::config::AllowsLocalReversibleActions(
                            applicationMode, layersConfig);
    executorConfig.priorityEnabled =
        r1Allowed && priorityConfig.enabled;
    executorConfig.priorityMaxLevel = priorityConfig.maxLevel;
    executorConfig.powerExecutionRequired =
        r1Allowed && powerConfig.executionRequired;
    // CFG-003：消费 [power].display_required（默认 false -> 零回归）。
    executorConfig.powerDisplayRequired =
        r1Allowed && powerConfig.displayRequired;
    executorConfig.consecutiveActionFailuresToHalt = static_cast<std::size_t>(
        std::max(0, policyConfig.haltAfterActionFailures));
    // AUD-001/002：R1 动作审计（观察者接入执行器每次后端调用）。
    // AUD-002：同时持久化到每用户审计文件（一行一条）；落盘失败如实计入失败计数。
    std::size_t auditWriteFailures = 0;
    const auto auditLogPath = DefaultAuditLogPath();
    optimizer::audit::AuditLog::Options auditOptions;
    auditOptions.filePath = auditLogPath;
    optimizer::audit::AuditLog auditLog(auditOptions);
    executorConfig.actionObserver =
        [&auditLog, &auditWriteFailures](
            const optimizer::policy::ExecutorActionEvent& event) {
            optimizer::audit::AuditRecord record;
            record.operationId = event.operationId;
            record.risk = optimizer::audit::RiskLevel::R1;
            record.caller = "policy";
            record.target = event.target;
            record.detail = event.detail;
            record.ok = event.ok;
            if (!auditLog.Append(std::move(record))) {
                ++auditWriteFailures; // 审计不可用：动作已发生但未能落盘
            }
        };
    optimizer::priority::PriorityBooster::Options boosterOptions;
    boosterOptions.maxLevel = executorConfig.priorityMaxLevel;
    auto powerLocker = std::make_shared<optimizer::power::PowerLocker>(
        optimizer::power::CreateWin32Backend());
    auto booster = std::make_shared<optimizer::priority::PriorityBooster>(
        optimizer::priority::CreateWin32Backend(), boosterOptions);
    optimizer::policy::PolicyExecutor executor(powerLocker, booster,
                                               executorConfig);
    const bool executionOn =
        executorConfig.priorityEnabled || executorConfig.powerExecutionRequired ||
        executorConfig.powerDisplayRequired;

    if (executionOn) {
        std::wcout
            << L"Policy decision window (advisory; R1 executors active)\n";
        std::wcout << L"  exec     : priority "
                   << (executorConfig.priorityEnabled ? L"on"
                                                      : L"off (gated)")
                   << L", power "
                   << (executorConfig.powerExecutionRequired
                           ? L"on"
                           : L"off (gated)")
                   << L" (display "
                   << (executorConfig.powerDisplayRequired ? L"on"
                                                           : L"off")
                   << L")";
        if (executorConfig.consecutiveActionFailuresToHalt > 0) {
            std::wcout << L", halt after "
                       << executorConfig.consecutiveActionFailuresToHalt
                       << L" consecutive action failures";
        }
        std::wcout << L"\n";
    } else {
        std::wcout
            << L"Policy decision (read-only advisory, no system changes)\n";
    }
    std::wcout << L"  rules : " << games.size() << L" game rule(s)\n";
    if (games.empty()) {
        std::wcout
            << L"  (pass --policy <s> <config.toml> to evaluate game rules)\n";
    }
    if (userAwayGateOn) {
        std::wcout << L"  user     : away when no input for >= "
                   << policyConfig.userAwayIdleSeconds
                   << L" s (presence gate on; read-only GetLastInputInfo)\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::size_t tick = 1;
    std::map<optimizer::policy::PolicyAction, int> counts;
    std::size_t execBoost = 0;
    std::size_t execUnboost = 0;
    std::size_t execPowerHold = 0;
    std::size_t execPowerRelease = 0;
    std::size_t execDisplayHold = 0;
    std::size_t execDisplayRelease = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();

        // 内存余量（只读查询；失败则本轮不决策）。
        std::optional<std::uint32_t> margin;
        if (auto status = optimizer::memory::QueryMemoryStatus(); status) {
            auto computed = optimizer::policy::ComputeMemoryMarginPercent(
                status.Value().availablePhysicalBytes,
                status.Value().totalPhysicalBytes);
            if (computed) {
                margin = computed.Value();
            }
        }

        // 游戏焦点与目标身份（单轮前台轮询，只读，不启动后台线程）。
        optimizer::policy::GameFocus focus;
        optimizer::policy::ExecutorTarget target;
        if (auto polled = watcher.PollOnce(); polled) {
            for (const auto& info : watcher.GetTrackedProcesses()) {
                if (info.state == optimizer::process::ProcessState::Running ||
                    info.state == optimizer::process::ProcessState::Starting) {
                    focus.gameId = info.gameId;
                    focus.running = true;
                    focus.foreground = info.isForeground;
                    target.gameId = info.gameId;
                    target.pid = info.pid;
                    target.creationTime100ns = info.creationTime100ns;
                    target.running = true;
                    const auto it = pauseByGame.find(info.gameId);
                    focus.pauseWhenBackground =
                        it == pauseByGame.end() ? true : it->second;
                    break; // v1：取配置顺序首个运行游戏
                }
            }
        }

        std::wostringstream line;
        optimizer::policy::PolicyDecision effectiveDecision;
        if (!margin) {
            // 无效/缺失指标：不触发新决策，但对账释放已持动作（游戏退出自动释放）。
            line << L"  [" << tick
                 << L"s] margin n/a (memory query unavailable; no decision)";
            optimizer::common::WriteConsoleLine(line.str());
        } else {
            optimizer::policy::PolicyInput input;
            auto pressure = optimizer::policy::ClassifyPressure(
                static_cast<std::int32_t>(*margin), thresholds);
            if (!pressure) {
                // 阈值非法（配置层已拦截，此处防御）：不决策。
                line << L"  [" << tick
                     << L"s] policy thresholds invalid; no decision";
                optimizer::common::WriteConsoleLine(line.str());
            } else {
                input.pressure = pressure.Value();
                input.game = focus;

                // 用户在场采样（ACT-004；仅门禁开启时）：每 tick 一次只读查询。
                // 查询失败视用户不在场（在场未知不伪装在场——不优化是安全方向）。
                bool userPresent = true;
                if (userAwayGateOn) {
                    userPresent = false;
                    if (auto inputQuery = lastInputBackend->Query()) {
                        const auto& inputSample = inputQuery.Value();
                        userPresent =
                            optimizer::activity::ClassifyActivity(
                                inputSample.nowTick,
                                inputSample.lastInputTick,
                                userAwayIdleMs) ==
                            optimizer::activity::ActivityState::Active;
                    }
                }
                input.userPresent = userPresent;

                const auto evaluation = evaluator.Evaluate(input, now);
                counts[evaluation.decision.action]++;
                effectiveDecision = evaluation.decision;

                line << L"  [" << tick << L"s] margin " << *margin << L"% "
                     << optimizer::policy::PressureToString(input.pressure)
                     << L" game=";
                if (focus.running) {
                    line << std::wstring(focus.gameId.begin(),
                                         focus.gameId.end())
                         << L" fg=" << (focus.foreground ? L"yes" : L"no");
                } else {
                    line << L"none";
                }
                if (userAwayGateOn) {
                    line << L" user="
                         << (userPresent ? L"present" : L"away");
                }
                line << L" -> "
                     << optimizer::policy::ActionToString(
                            evaluation.decision.action)
                     << L" ("
                     << std::wstring(evaluation.decision.reasonCode.begin(),
                                     evaluation.decision.reasonCode.end());
                if (evaluation.suppressed) {
                    line << L", cooldown";
                }
                line << L")";
                optimizer::common::WriteConsoleLine(line.str());
            }
        }

        // 决策落地（含 margin 无效时的对账释放）。
        if (const auto applied = executor.ApplyDecision(effectiveDecision,
                                                        target)) {
            const auto& effect = applied.Value();
            if (effect.priorityBoosted) {
                std::wostringstream out;
                out << L"  [exec] priority boosted: "
                    << std::wstring(target.gameId.begin(), target.gameId.end())
                    << L" pid=" << target.pid;
                optimizer::common::WriteConsoleLine(out.str());
                ++execBoost;
            }
            if (effect.priorityReleased) {
                std::wostringstream out;
                out << L"  [exec] priority released: "
                    << std::wstring(target.gameId.begin(), target.gameId.end());
                optimizer::common::WriteConsoleLine(out.str());
                ++execUnboost;
            }
            if (effect.powerHeld) {
                optimizer::common::WriteConsoleLine(
                    L"  [exec] power request held (game running)");
                ++execPowerHold;
            }
            if (effect.powerReleased) {
                optimizer::common::WriteConsoleLine(
                    L"  [exec] power request released");
                ++execPowerRelease;
            }
            if (effect.displayHeld) {
                optimizer::common::WriteConsoleLine(
                    L"  [exec] display power request held (game running)");
                ++execDisplayHold;
            }
            if (effect.displayReleased) {
                optimizer::common::WriteConsoleLine(
                    L"  [exec] display power request released");
                ++execDisplayRelease;
            }
            if (!effect.skipped.empty()) {
                std::wostringstream out;
                out << L"  [exec] skipped: " << effect.skipped;
                optimizer::common::WriteConsoleLine(out.str());
            }
        }

        if (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ++tick;
    }

    executor.ReleaseAll(); // 退出前释放全部已持动作（尽力而为）

    std::wostringstream summary;
    summary << L"  summary : NoOp "
            << counts[optimizer::policy::PolicyAction::NoOp] << L" / Notify "
            << counts[optimizer::policy::PolicyAction::Notify]
            << L" / SuggestMemoryTune "
            << counts[optimizer::policy::PolicyAction::SuggestMemoryTune]
            << L" / SuggestPriorityBoost "
            << counts[optimizer::policy::PolicyAction::SuggestPriorityBoost];
    optimizer::common::WriteConsoleLine(summary.str());
    if (executionOn) {
        std::wostringstream execSummary;
        execSummary << L"  exec     : boost " << execBoost << L" / unboost "
                    << execUnboost << L" / power+ " << execPowerHold
                    << L" / power- " << execPowerRelease;
        if (execDisplayHold > 0 || execDisplayRelease > 0) {
            execSummary << L" / display+ " << execDisplayHold
                        << L" / display- " << execDisplayRelease;
        }
        optimizer::common::WriteConsoleLine(execSummary.str());
    }
    if (auditLog.Size() > 0) {
        std::wostringstream auditLine;
        auditLine << L"  audit    : " << auditLog.Size()
                  << L" R1 action record(s) appended to "
                  << auditLogPath.wstring();
        optimizer::common::WriteConsoleLine(auditLine.str());
        for (const auto& record : auditLog.Records()) {
            optimizer::common::WriteConsoleLine(
                optimizer::audit::FormatAuditRecord(record));
        }
    }
    if (auditWriteFailures > 0) {
        // 审计不可用：不把失败当成功（R2/R3 门禁将以此为拒绝依据，见门禁设计）。
        std::wostringstream auditFailLine;
        auditFailLine << L"  audit    : " << auditWriteFailures
                      << L" record(s) failed to persist (audit unavailable)";
        optimizer::common::WriteConsoleLine(auditFailLine.str());
    }
    return 0;
}

// 登录后拉起的宿主命令行：自启动项与计划任务共用（仅一处真相，避免两处漂移）。
std::wstring HostAutostartCommandLine(const std::wstring& exePath) {
    return L"\"" + exePath + L"\" --service console run --tray --ipc-facts";
}

// 解析当前可执行文件路径；失败返回 false。
bool ResolveExecutablePath(std::wstring& exePath) {
    exePath.assign(MAX_PATH, L' ');
    const DWORD written = ::GetModuleFileNameW(
        nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    if (written == 0 || written >= exePath.size()) {
        return false;
    }
    exePath.resize(written);
    return true;
}

int RunScheduledTaskCommand(int argc, wchar_t* argv[]) {
    // --scheduled-task <status|install|remove> [taskname]：每用户计划任务（登录触发、当前用户交互令牌）。
    // 安全契约：注册需要管理员（非提升进程得到拒绝访问并如实报错）；不请求提升、不设置最高运行级别；
    // 默认不自动安装（须显式命令）；remove 幂等；"开机触发" 被显式拒绝（需 SYSTEM，与权限分界冲突）。
    if (argc < 3) {
        ErrorLine{} << L"  --scheduled-task requires status|install|remove\n";
        return 2;
    }
    const std::wstring_view action(argv[2]);
    const std::wstring taskName =
        argc >= 4 ? argv[3] : optimizer::service::kScheduledTaskName;
    if (action == L"status") {
        const auto queried = optimizer::service::QueryScheduledTask(taskName);
        if (!queried) {
            const auto& error = queried.ErrorValue();
            ErrorLine{} << L"  scheduled task query failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        optimizer::common::WriteConsoleLine(
            L"Scheduled task (logon trigger, current user token)");
        {
            std::wostringstream line;
            line << L"  task     : " << taskName;
            optimizer::common::WriteConsoleLine(line.str());
        }
        if (!queried.Value().installed) {
            optimizer::common::WriteConsoleLine(
                L"  state    : not installed");
            return 0;
        }
        {
            std::wostringstream line;
            line << L"  state    : installed";
            if (!queried.Value().trigger.empty()) {
                line << L" (trigger " << queried.Value().trigger << L")";
            }
            optimizer::common::WriteConsoleLine(line.str());
        }
        if (!queried.Value().commandLine.empty()) {
            optimizer::common::WriteConsoleLine(
                L"  command  : " + queried.Value().commandLine);
        } else {
            optimizer::common::WriteConsoleLine(
                L"  command  : unavailable (task exists but its action could not be read)");
        }
        return 0;
    }
    if (action == L"install") {
        std::wstring exePath;
        if (!ResolveExecutablePath(exePath)) {
            ErrorLine{} << L"  cannot resolve current executable path\n";
            return 2;
        }
        const std::wstring command = HostAutostartCommandLine(exePath);
        optimizer::service::ScheduledTaskSpec spec;
        spec.taskName = taskName;
        spec.commandLine = command;
        spec.trigger = optimizer::service::TaskTrigger::OnLogon;
        JournalAction("agent.form_install", "task", true,
                      optimizer::audit::JournalPhase::Before,
                      "intent: install scheduled task");
        const auto installed = optimizer::service::InstallScheduledTask(spec);
        if (!installed) {
            const auto& error = installed.ErrorValue();
            ErrorLine{} << L"  scheduled task install failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            ErrorLine{} << L"  hint     : registering a per-machine task needs elevated rights "
                           L"(one-time UAC); the task itself never runs elevated\n";
            return 2;
        }
        AuditAgentFormAction(optimizer::service::AgentForm::ScheduledTask, true,
                             true);
        optimizer::common::WriteConsoleLine(
            L"  scheduled: installed (per-user, logon trigger) -> " + command);
        optimizer::common::WriteConsoleLine(
            L"  revert   : CppOptimizer.exe --scheduled-task remove");
        return 0;
    }
    if (action == L"remove") {
        JournalAction("agent.form_remove", "task", true,
                      optimizer::audit::JournalPhase::Before,
                      "intent: remove scheduled task");
        const auto removed = optimizer::service::RemoveScheduledTask(taskName);
        if (!removed) {
            const auto& error = removed.ErrorValue();
            ErrorLine{} << L"  scheduled task remove failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        AuditAgentFormAction(optimizer::service::AgentForm::ScheduledTask, false,
                             true);
        optimizer::common::WriteConsoleLine(
            L"  scheduled: removed (idempotent)");
        return 0;
    }
    ErrorLine{} << L"  --scheduled-task requires status|install|remove\n";
    return 2;
}

int RunStartupCommand(int argc, wchar_t* argv[]) {
    // --startup <status|install|remove>：每用户自启动项（HKCU Run 键）。
    // R1 局部可逆：仅当前用户、标准用户即可、remove 即恢复；默认不自动安装（须显式命令）。
    if (argc < 3) {
        ErrorLine{} << L"  --startup requires status|install|remove\n";
        return 2;
    }
    const std::wstring_view action(argv[2]);
    if (action == L"status") {
        const auto queried = optimizer::service::QueryStartupEntry();
        if (!queried) {
            const auto& error = queried.ErrorValue();
            ErrorLine{} << L"  startup query failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        optimizer::common::WriteConsoleLine(L"Startup entry (per-user, HKCU Run)");
        optimizer::common::WriteConsoleLine(
            queried.Value().empty()
                ? L"  state    : not installed"
                : L"  state    : installed -> " + queried.Value());
        return 0;
    }
    if (action == L"install") {
        std::wstring exePath;
        if (!ResolveExecutablePath(exePath)) {
            ErrorLine{} << L"  cannot resolve current executable path\n";
            return 2;
        }
        // AGENT-A2：启动项必须写入**能拉起常驻托盘宿主的命令行**（仅写 exe 路径的话，
        // 登录后启动的进程无参数 -> 只打印 usage，起不到 Agent 形态的作用）。
        auto backend = optimizer::service::CreateWin32StartupEntryBackend();
        if (!backend) {
            ErrorLine{} << L"  startup install failed: 无法创建注册表后端\n";
            return 2;
        }
        const std::wstring command = HostAutostartCommandLine(exePath);
        JournalAction("agent.form_install", "startup_tray", true,
                      optimizer::audit::JournalPhase::Before,
                      "intent: install per-user startup entry");
        const auto installed = backend->Write(command);
        if (!installed) {
            const auto& error = installed.ErrorValue();
            ErrorLine{} << L"  startup install failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        AuditAgentFormAction(optimizer::service::AgentForm::StartupTray, true,
                             true);
        optimizer::common::WriteConsoleLine(
            L"  startup  : installed (per-user) -> " + command);
        optimizer::common::WriteConsoleLine(
            L"  revert   : CppOptimizer.exe --startup remove");
        return 0;
    }
    if (action == L"remove") {
        JournalAction("agent.form_remove", "startup_tray", true,
                      optimizer::audit::JournalPhase::Before,
                      "intent: remove per-user startup entry");
        const auto removed = optimizer::service::RemoveStartupEntry();
        if (!removed) {
            const auto& error = removed.ErrorValue();
            ErrorLine{} << L"  startup remove failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        AuditAgentFormAction(optimizer::service::AgentForm::StartupTray, false,
                             true);
        optimizer::common::WriteConsoleLine(L"  startup  : removed (idempotent)");
        return 0;
    }
    ErrorLine{} << L"  --startup requires status|install|remove\n";
    return 2;
}

int RunPowerLockCommand(int argc, wchar_t* argv[]) {
    // --power-lock <s> [execution|display|both] [reason...]：
    // R1 局部可逆演示命令——前台有界持有电源请求（阻止睡眠/熄屏），
    // 到点自动释放；进程退出时句柄随进程句柄表关闭，系统侧请求自动取消。
    // Power Request 表达睡眠/显示需求，不承诺锁定 CPU/GPU 频率。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (!ParseUint32(argv[2], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --power-lock seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }

    // 类型：execution（默认）/ display / both（ASCII 大小写不敏感）。
    std::vector<optimizer::power::PowerLockType> types{
        optimizer::power::PowerLockType::ExecutionRequired};
    int index = 3;
    if (argc > index) {
        const std::wstring_view first(argv[index]);
        if (AsciiEqualsIgnoreCaseW(first, L"both")) {
            types = {optimizer::power::PowerLockType::ExecutionRequired,
                     optimizer::power::PowerLockType::DisplayRequired};
            ++index;
        } else {
            auto utf8 = optimizer::common::WideToUtf8(first);
            if (utf8) {
                auto parsed =
                    optimizer::power::ParsePowerLockType(utf8.Value());
                if (parsed) {
                    types = {parsed.Value()};
                    ++index;
                }
            }
        }
    }

    // reason：剩余参数以空格连接（默认内置说明）。
    std::wstring reason = L"CppOptimizer bounded power-lock demo (R1)";
    if (argc > index) {
        reason.clear();
        for (int i = index; i < argc; ++i) {
            if (i > index) {
                reason += L' ';
            }
            reason += argv[i];
        }
    }

    auto backend = optimizer::power::CreateWin32Backend();
    optimizer::power::PowerLocker locker(backend);

    std::wcout
        << L"Power lock (R1, reversible; system state restored on exit)\n";
    std::wcout << L"  type(s) :";
    for (const auto type : types) {
        std::wcout << L" " << optimizer::power::PowerLockTypeToString(type);
    }
    std::wcout << L"\n  reason  : " << reason << L"\n";

    for (const auto type : types) {
        JournalAction("power.hold", "power-lock(cli)", true,
                      optimizer::audit::JournalPhase::Before,
                      "intent: acquire bounded power request");
        const auto acquired = locker.AcquireLock(type, reason);
        if (!acquired) {
            const auto& error = acquired.ErrorValue();
            ErrorLine{}
                << L"  acquire "
                << optimizer::power::PowerLockTypeToString(type)
                << L" failed [" << optimizer::common::ToString(error.domain)
                << L":" << error.code << L"] " << error.message << L"\n";
            AuditAction("power.hold", optimizer::audit::RiskLevel::R1,
                        "power-lock(cli)", false,
                        "acquire failed: " + error.operation,
                        "no power request held");
            locker.ReleaseAll();
            return 2;
        }
        AuditAction("power.hold", optimizer::audit::RiskLevel::R1,
                    "power-lock(cli)", true, "bounded foreground hold (cli)",
                    "power request(s) held (bounded)");
        std::wcout << L"  [acquired] "
                   << optimizer::power::PowerLockTypeToString(type) << L"\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::wcout << L"  holding for " << seconds << L" s ...\n";
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    JournalAction("power.release", "power-lock(cli)", true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: release bounded power requests");
    locker.ReleaseAll();
    AuditAction("power.release", optimizer::audit::RiskLevel::R1,
                "power-lock(cli)", true, "released on bounded exit (cli)",
                "all power requests released");
    std::wcout << L"  [released] all power requests (sleep/display restored)\n";
    return 0;
}

int RunPriorityBoostCommand(int argc, wchar_t* argv[]) {
    // --priority-boost <s> <pid> [config.toml]：
    // R1 局部可逆演示命令——对指定进程临时提升优先级类，到点按"条件恢复"还原：
    // 仅当进程仍同实例且当前优先级未被外部改动时才恢复原值（不覆盖外部修改）；
    // 进程退出时提升随进程消失（目标退出视为正常取消）。
    // 等级取自配置 [priority].max_level（默认 above_normal），High 需显式配置。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (!ParseUint32(argv[2], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --priority-boost seconds must be in 1.."
                   << kMaxSeconds << L"\n";
        return 2;
    }
    std::uint32_t pid = 0;
    if (argc < 4 || !ParseUint32(argv[3], pid) || pid == 0) {
        ErrorLine{} << L"  --priority-boost requires a positive pid\n";
        return 2;
    }

    optimizer::config::PriorityConfig priorityConfig;
    if (argc >= 5) {
        const std::filesystem::path mainPath(argv[4]);
        const std::wstring localPath =
            (mainPath.parent_path() / L"config.local.toml").wstring();
        auto config =
            optimizer::config::LoadConfigWithLocal(argv[4], localPath);
        if (!config) {
            const auto& error = config.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        priorityConfig = config.Value().priority;
    }
    const auto level = priorityConfig.maxLevel;
    if (level == optimizer::config::PriorityLevel::None) {
        ErrorLine{} << L"  [priority].max_level is \"none\"; nothing to boost\n";
        return 2;
    }

    // 进程名（尽力而为，只读；权限不足时为空）。
    std::wstring processName;
    if (const auto details = optimizer::process::QueryProcessDetails(pid)) {
        processName = details.Value().name;
    }

    const wchar_t* levelName = level == optimizer::config::PriorityLevel::High
                                   ? L"high"
                                   : L"above_normal";
    optimizer::priority::PriorityBooster::Options options;
    options.maxLevel = level;
    auto backend = optimizer::priority::CreateWin32Backend();
    optimizer::priority::PriorityBooster booster(backend, options);

    std::wcout
        << L"Priority boost (R1, reversible; priority restored on exit)\n";
    std::wcout << L"  target   : pid " << pid;
    if (!processName.empty()) {
        std::wcout << L" (" << processName << L")";
    }
    std::wcout << L"\n  level    : " << levelName << L"\n";
    std::wcout << L"  enabled  : "
               << (priorityConfig.enabled
                       ? L"yes"
                       : L"no (policy automation off; manual demo still applies)")
               << L"\n";

    JournalAction("priority.boost", "pid " + std::to_string(pid), true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: acquire bounded priority lease");
    const auto acquired =
        booster.AcquireBoost("cli-demo", pid, 0, level);
    if (!acquired) {
        const auto& error = acquired.ErrorValue();
        AuditAction("priority.boost", optimizer::audit::RiskLevel::R1,
                    "pid " + std::to_string(pid), false,
                    "acquire failed: " + error.operation,
                    "priority unchanged");
        ErrorLine{} << L"  acquire failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    std::wcout << L"  [boosted] pid " << pid << L" -> " << levelName
               << L"\n";

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::wcout << L"  holding for " << seconds << L" s ...\n";
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    JournalAction("priority.unboost", "pid " + std::to_string(pid), true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: release priority lease (conditional restore)");
    AuditAction("priority.boost", optimizer::audit::RiskLevel::R1,
                "pid " + std::to_string(pid), true, "bounded lease (cli)",
                "priority boosted (bounded lease)");
    AuditAction("priority.unboost", optimizer::audit::RiskLevel::R1,
                "pid " + std::to_string(pid), true, "bounded lease exit (cli)",
                "priority restored (conditional)");
    booster.ReleaseAll();
    std::wcout
        << L"  [restored] priority (conditional restore; external changes "
           L"never overwritten)\n";
    return 0;
}

int RunListProcesses(int argc, wchar_t* argv[]) {
    // --list-processes [--all] [filter]: 只读进程目录，默认只列有可见窗口的进程。
    // 供用户辨认并挑选要添加为游戏的进程；无任何系统修改。
    bool windowOnly = true;
    std::wstring filter;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--all") {
            windowOnly = false;
        } else if (filter.empty()) {
            filter = argv[i]; // 首个非开关参数作为子串过滤
        }
    }

    auto result =
        optimizer::process::EnumerateProcessDetails(windowOnly);
    if (!result) {
        const auto& error = result.ErrorValue();
        ErrorLine{} << L"  process list failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    // 过滤（子串匹配进程名/路径/窗口标题）。
    std::vector<optimizer::process::ProcessDetails> details;
    details.reserve(result.Value().size());
    for (const auto& item : result.Value()) {
        if (optimizer::process::ProcessMatchesFilter(item, filter)) {
            details.push_back(item);
        }
    }
    // 按 pid 升序，输出稳定。
    std::sort(details.begin(), details.end(),
              [](const auto& a, const auto& b) { return a.pid < b.pid; });

    constexpr int kNameWidth = 30;
    constexpr int kPathWidth = 52;
    constexpr int kTitleWidth = 26;

    // 用 wostringstream 构建行（纯内存，无 locale 转换），再经双路径控制台输出。
    std::wostringstream line;
    line << L"Process list (read-only"
         << (windowOnly ? L", visible windows" : L", all") << L")\n";
    optimizer::common::WriteConsoleLine(line.str());
    line.str(L"");
    line.clear();

    line << L"  pid     " << L"name"
         << std::wstring(kNameWidth - 4, L' ') << L"path"
         << std::wstring(kPathWidth - 4, L' ') << L"window title"
         << std::wstring(kTitleWidth - 12, L' ') << L"memory";
    optimizer::common::WriteConsoleLine(line.str());

    const auto trim = [](const std::wstring& text, std::size_t width) {
        return text.size() <= width ? text : text.substr(0, width);
    };
    for (const auto& item : details) {
        line.str(L"");
        line.clear();
        line << L"  " << std::setw(7) << item.pid << L"  "
             << std::setw(kNameWidth) << std::left
             << trim(item.name, kNameWidth) << L" "
             << std::setw(kPathWidth) << std::left
             << trim(item.executablePath, kPathWidth) << L" "
             << std::setw(kTitleWidth) << std::left
             << trim(item.windowTitle, kTitleWidth) << L" "
             << (item.isForeground ? L"[FG] " : L"")
             << optimizer::memory::FormatBytes(item.workingSetBytes);
        optimizer::common::WriteConsoleLine(line.str());
    }

    line.str(L"");
    line.clear();
    line << L"  " << details.size() << L" process(es)";
    optimizer::common::WriteConsoleLine(line.str());
    return 0;
}

int RunDiagnostics() {
    std::wcout << L"CppOptimizer diagnostics\n";
    std::wcout << L"  process mode : user mode (Ring 3)\n";
#if defined(_M_X64)
    std::wcout << L"  architecture : x64\n";
#elif defined(_M_IX86)
    std::wcout << L"  architecture : x86 (not a release target)\n";
#else
    std::wcout << L"  architecture : unsupported/unknown\n";
#endif
    std::wcout << L"  safe default : observe only\n";

    auto result = optimizer::platform::NativeApi::Instance().Probe();
    if (!result) {
        const auto& error = result.ErrorValue();
        ErrorLine{} << L"  native probe : failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& capabilities = result.Value();
    std::wcout << L"  ntdll loaded : " << (capabilities.ntdllLoaded ? L"yes" : L"no") << L"\n";
    std::wcout << L"  NtQuerySystemInformation : "
               << (capabilities.querySystemInformation ? L"available" : L"unavailable") << L"\n";
    std::wcout << L"  NtSetSystemInformation   : "
               << (capabilities.setSystemInformation ? L"available (write remains disabled)" : L"unavailable")
               << L"\n";
    std::wcout << L"  RtlNtStatusToDosError    : "
               << (capabilities.ntStatusConversion ? L"available" : L"unavailable") << L"\n";
    return 0;
}

// --add-game 的 local 配置路径：main 同目录 config.local.toml；无 main 时当前目录。
std::wstring LocalConfigPath(std::wstring_view mainPath) {
    if (mainPath.empty()) {
        return L"config.local.toml";
    }
    const std::filesystem::path main(mainPath);
    return (main.parent_path() / L"config.local.toml").wstring();
}

// 读取一行宽字符输入（交互模式）。
std::wstring ReadConsoleLine() {
    std::wstring line;
    std::getline(std::wcin, line);
    return line;
}

// 打印一条游戏规则的预览。
void PrintRulePreview(const optimizer::config::GameConfig& game) {
    std::wostringstream line;
    line << L"  id             : "
         << std::wstring(game.id.begin(), game.id.end());
    optimizer::common::WriteConsoleLine(line.str());
    line.str(L"");
    line.clear();
    line << L"  display_name   : "
         << std::wstring(game.displayName.begin(), game.displayName.end());
    optimizer::common::WriteConsoleLine(line.str());
    line.str(L"");
    line.clear();
    line << L"  process_names  : [";
    for (std::size_t i = 0; i < game.processNames.size(); ++i) {
        if (i > 0) {
            line << L", ";
        }
        line << L"\""
             << std::wstring(game.processNames[i].begin(),
                             game.processNames[i].end())
             << L"\"";
    }
    line << L"]";
    optimizer::common::WriteConsoleLine(line.str());
    line.str(L"");
    line.clear();
    line << L"  pause_when_background : "
         << (game.pauseWhenBackground ? L"true" : L"false");
    optimizer::common::WriteConsoleLine(line.str());
}

// 现有规则（用于 id 去重）：main + local 合并，或仅 local。
optimizer::common::Result<std::vector<optimizer::config::GameConfig>>
LoadExistingGameRules(std::wstring_view mainPath,
                      std::wstring_view localPath) {
    if (!mainPath.empty()) {
        auto config =
            optimizer::config::LoadConfigWithLocal(mainPath, localPath);
        if (!config) {
            return optimizer::common::Result<
                std::vector<optimizer::config::GameConfig>>::Failure(
                config.ErrorValue());
        }
        return optimizer::common::Result<
            std::vector<optimizer::config::GameConfig>>::Success(
            std::move(config.Value().games));
    }
    std::error_code existsError;
    if (std::filesystem::exists(std::filesystem::path(localPath),
                                existsError)) {
        auto config = optimizer::config::LoadConfig(localPath);
        if (!config) {
            return optimizer::common::Result<
                std::vector<optimizer::config::GameConfig>>::Failure(
                config.ErrorValue());
        }
        return optimizer::common::Result<
            std::vector<optimizer::config::GameConfig>>::Success(
            std::move(config.Value().games));
    }
    return optimizer::common::Result<
        std::vector<optimizer::config::GameConfig>>::Success({});
}

int RunAddGameCommand(int argc, wchar_t* argv[]) {
    // --add-game [pid] [main.toml] [--dry-run]：从当前运行进程生成游戏规则
    // 并写入 config.local.toml（用户自建配置，与预设 main 配置分离）。
    // 交互模式（未给 pid）：列出有窗口进程 -> 输编号 -> 预览 -> 确认 -> 写入；
    // 非交互模式（给了 pid）：直接预览并写入（显式指定即明确意图）。
    // 写入是唯一的本地文件操作（原子写），无任何系统级动作。
    std::uint32_t pid = 0;
    std::wstring mainPath;
    bool dryRun = false;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--dry-run") {
            dryRun = true;
        } else if (ParseUint32(argv[i], pid) && pid > 0) {
            // 纯数字参数 -> pid（非交互模式）。
        } else if (mainPath.empty()) {
            mainPath = argv[i];
        } else {
            ErrorLine{} << L"  unexpected argument: " << argv[i] << L"\n";
            return 2;
        }
    }

    // 确定选中的进程。
    std::optional<optimizer::process::ProcessDetails> selected;
    if (pid == 0) {
        // 交互模式：列出有可见窗口的进程。
        auto result = optimizer::process::EnumerateProcessDetails(true);
        if (!result) {
            const auto& error = result.ErrorValue();
            ErrorLine{} << L"  process list failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        auto list = std::move(result.Value());
        std::sort(list.begin(), list.end(),
                  [](const auto& a, const auto& b) { return a.pid < b.pid; });
        if (list.empty()) {
            optimizer::common::WriteConsoleLine(
                L"  没有可添加的有窗口进程（无窗口进程请用 --add-game <pid>）");
            return 0;
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            std::wostringstream line;
            line << L"  [" << (i + 1) << L"] pid=" << list[i].pid << L"  "
                 << list[i].name << L"  " << list[i].windowTitle;
            optimizer::common::WriteConsoleLine(line.str());
        }
        for (;;) {
            optimizer::common::WriteConsoleLine(L"  输入编号添加（q 退出）：");
            const std::wstring input = ReadConsoleLine();
            if (input.empty() || input == L"q" || input == L"Q") {
                return 0;
            }
            std::uint32_t index = 0;
            if (!ParseUint32(input, index) || index == 0 ||
                index > list.size()) {
                optimizer::common::WriteConsoleLine(L"  无效编号，请重新输入");
                continue;
            }
            selected = list[index - 1];
            break;
        }
    } else {
        auto query = optimizer::process::QueryProcessDetails(pid);
        if (!query) {
            const auto& error = query.ErrorValue();
            ErrorLine{} << L"  query pid failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        selected = query.Value();
    }

    // 生成规则（id 去重基于 main + local 的最终规则集）。
    const std::wstring localPath = LocalConfigPath(mainPath);
    auto existing = LoadExistingGameRules(mainPath, localPath);
    if (!existing) {
        const auto& error = existing.ErrorValue();
        ErrorLine{} << L"  config load failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    auto rule = optimizer::process::BuildGameRuleFromProcess(
        selected.value(), existing.Value());
    if (!rule) {
        const auto& error = rule.ErrorValue();
        ErrorLine{} << L"  rule build failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    optimizer::common::WriteConsoleLine(L"将添加游戏规则：");
    PrintRulePreview(rule.Value());

    if (dryRun) {
        optimizer::common::WriteConsoleLine(L"  --dry-run：未写入任何文件");
        return 0;
    }
    if (pid == 0) {
        std::wostringstream prompt;
        prompt << L"  确认写入 " << localPath << L"？[y/N] ";
        optimizer::common::WriteConsoleLine(prompt.str());
        const std::wstring answer = ReadConsoleLine();
        if (answer != L"y" && answer != L"Y") {
            optimizer::common::WriteConsoleLine(L"  已取消");
            return 0;
        }
    }

    std::vector<optimizer::config::GameConfig> toWrite{rule.Value()};
    auto append = optimizer::config::AppendGameRules(localPath, toWrite);
    if (!append) {
        const auto& error = append.ErrorValue();
        ErrorLine{} << L"  write failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    std::wostringstream done;
    done << L"  已添加规则 -> " << localPath;
    optimizer::common::WriteConsoleLine(done.str());
    if (!mainPath.empty()) {
        std::wostringstream hint;
        hint << L"  验证：CppOptimizer.exe --watch 10 " << mainPath;
        optimizer::common::WriteConsoleLine(hint.str());
    }
    return 0;
}

// 服务宿主：服务名（SCM 以 argv[1]==服务名 启动本进程，与 --service service 等价）。
constexpr wchar_t kServiceName[] = L"CppOptimizerService";
constexpr wchar_t kServiceDisplayName[] = L"CppOptimizer Service";
constexpr wchar_t kServiceDescription[] =
    L"CppOptimizer 只读观测与受控优化宿主（R0 负载）";

// 服务宿主负载状态：R0 只读观测（每 tick 一次内存快照 + Info 日志），可选
// “受保护管道事实消费”（SVC-002/003，仅 console demo 启用）：作为 IPC 服务端
// 常驻监听（persistentAccept），窗口内连续受理到达的 Agent 客户端（每客户端一帧
// FactsSnapshot）并记录身份/摘要。
// 服务模式不接受临时危险命令，本负载不产生任何系统修改。
// SVC-006：宿主在场台账默认“不在场”空闲阈值（秒），与 --activity 默认空闲阈值一致。
constexpr std::uint32_t kHostPresenceAwaySeconds = 15;

struct ServiceHostDemoState {
    optimizer::logger::Logger logger;
    std::size_t tickCount = 0;

    // IPC 事实消费（SVC-002/003）：
    bool ipcEnabled = false;                      // console demo 经 --ipc-facts 开启
    std::wstring ipcPipeName;                     // 受保护管道名
    std::shared_ptr<optimizer::ipc::IpcSession> ipcSession; // 常驻监听会话
    std::size_t ipcClientsServed = 0;             // 窗口内已受理客户端数
    std::wstring ipcLastIdentity;                 // 最近受理客户端“pid/session/user”
    std::size_t ipcLastPayloadBytes = 0;
    std::size_t ipcLastFactCount = 0;
    bool ipcLastReplyAck = false;                 // 最近一次是否回 Ack
    std::string ipcLastFactSummary;               // 最近受理解析摘要（ASCII）
    // ACT-006：最近受理 Agent 上报的用户活动观测（user_idle_seconds 已注册数值键；
    // 未上报/不可解析为空，窗口汇总按 n/a 展示）。
    std::optional<std::uint32_t> ipcLastUserIdleSeconds;
    // IPC-019：最近受理 Agent 上报的前台窗口所属 pid（无前台/未上报为空）。
    std::optional<std::uint32_t> ipcLastForegroundPid;
    std::wstring ipcExpectedToken;                // 会话凭据（IPC-005；空 = 不要求）
    // Safe Mode（IPC-010/013，Agent 受理门禁）：时间窗口内身份/凭据失败达阈值暂停受理新 Agent。
    std::optional<optimizer::service::SafeModeGuard> ipcSafeMode; // console demo 启用
    bool ipcAuthRejected = false;                 // 最近一次 ServeOne 是否以身份/凭据拒绝结束
    std::size_t ipcSafeModeEntries = 0;           // 窗口内进入 Safe Mode 次数（汇总）
    std::size_t ipcRejectedClients = 0;           // 窗口内身份/凭据拒绝客户端数（汇总）
    // IPC-015/016：离散异常触发（Native capability 探测异常、不支持 OS/build）——启动只读检查，
    // 异常锁存 Safe Mode。
    bool ipcNativeProbeOk = false;                // 启动探测通过（R0 baseline）
    bool ipcNativeProbeAnomaly = false;           // Native 探测异常（已触发锁存）
    bool ipcOsSupportAnomaly = false;             // 操作系统不支持（已触发锁存）
    // SVC-004：每用户默认配置无效（启动自动消费时解析失败）-> Safe Mode「配置无效」离散触发。
    bool ipcConfigAnomaly = false;                // 默认配置无效（已触发锁存）
    // IPC-018：上次异常退出且恢复未确认（recovery-state.json）——启动发现标记即锁存 Safe Mode。
    bool ipcRecoveryUnconfirmed = false;          // 上次会话未正常结束且未确认（已锁存）
    bool ipcRecoveryNotedOnly = false;            // 同上，但配置关闭了阻断（可见不阻断，未锁存）
    std::size_t ipcAnomalyEntries = 0;            // 窗口内离散异常触发次数（汇总）
    // SVC-006/007：宿主在场台账（Agent user_idle 汇总；仅展示/记录，不参与 Safe Mode 触发判定）。
    std::optional<optimizer::service::HostPresenceTracker> ipcPresence;
    // SVC-008：宿主本机事件驱动输入（--tray 隐藏窗口 Raw Input；无窗口时轮询兜底）。
    std::shared_ptr<optimizer::activity::RawInputEventSource> rawInput;
    bool rawInputAttached = false; // 事件源已绑定并注册原始输入
    std::shared_ptr<optimizer::activity::LastInputBackend> lastInputFallback;
    // SVC-009：在场汇总变化次数（变化事件回调计数，供窗口汇总展示）。
    std::size_t presenceTransitions = 0;
};

optimizer::common::Result<void> ServiceWorkloadTick(
    ServiceHostDemoState& state, bool memoryQueryEnabled = true) noexcept {
    // CFG-006：[memory].query_enabled = false -> 不查询，且如实拒绝（不把“未查询”伪装成
    // “查询失败”或报 0）。
    if (!memoryQueryEnabled) {
        return optimizer::common::Result<void>::Failure(
            optimizer::common::Error::Unsupported(
                "ServiceWorkloadTick",
                L"内存查询已被配置关闭（[memory].query_enabled = false）"));
    }
    auto status = optimizer::memory::QueryMemoryStatus();
    if (!status) {
        return optimizer::common::Result<void>::Failure(status.ErrorValue());
    }
    const auto& s = status.Value();
    ++state.tickCount;
    std::wstring message =
        L"tick " + std::to_wstring(state.tickCount) + L": available " +
        optimizer::memory::FormatBytes(s.availablePhysicalBytes) + L", load " +
        std::to_wstring(s.memoryLoadPercent) + L"%";
    state.logger.Write(optimizer::logger::LogLevel::Info, L"service", message);

    // SVC-008：宿主本机在场（事件驱动输入优先；尚无事件/未绑定时回退 GetLastInputInfo
    // 轮询；查询失败记 Unknown 不伪装）。放在受理门禁之前，Safe Mode 暂停期也持续更新。
    if (state.ipcPresence && state.rawInput) {
        std::optional<std::uint32_t> idleSeconds;
        if (state.rawInputAttached) {
            if (auto idle = state.rawInput->IdleDuration()) {
                idleSeconds =
                    static_cast<std::uint32_t>(idle->count() / 1000);
            }
        }
        if (!idleSeconds && state.lastInputFallback) {
            auto sample = state.lastInputFallback->Query();
            if (sample) {
                const std::int64_t idleMs =
                    optimizer::activity::IdleMilliseconds(
                        sample.Value().nowTick, sample.Value().lastInputTick);
                idleSeconds =
                    static_cast<std::uint32_t>(idleMs / 1000);
            }
        }
        (void)state.ipcPresence->Record("host", idleSeconds);
    }

    // SVC-002/003：受保护管道事实消费（--ipc-facts；常驻监听连续受理多客户端）。
    if (state.ipcEnabled && state.ipcSession) {
        state.ipcAuthRejected = false; // 每轮受理前复位（verdictObserver 按结论置位）
        // Safe Mode（IPC-010）：冷却期暂停受理新 Agent，R0 观测/日志照常。
        if (state.ipcSafeMode && !state.ipcSafeMode->ShouldAcceptClients()) {
            if (state.ipcSafeMode->IsAnomalyLatched()) {
                std::wstring sources;
                const auto append = [&sources](bool on, const wchar_t* name) {
                    if (on) {
                        if (!sources.empty()) {
                            sources += L"/";
                        }
                        sources += name;
                    }
                };
                append(state.ipcNativeProbeAnomaly, L"native");
                append(state.ipcOsSupportAnomaly, L"os");
                append(state.ipcConfigAnomaly, L"config");
                append(state.ipcRecoveryUnconfirmed, L"recovery");
                state.logger.Write(
                    optimizer::logger::LogLevel::Info, L"service",
                    L"ipc  : Safe Mode（启动异常锁存：" + sources +
                        L"）保持，暂停受理 Agent");
            } else {
                state.logger.Write(optimizer::logger::LogLevel::Info,
                                   L"service",
                                   L"ipc  : Safe Mode 冷却中，暂停受理 Agent");
            }
            // SVC-009：暂停期也推进在场汇总（宿主事件输入行仍可能更新）。
            if (state.ipcPresence) {
                (void)state.ipcPresence->Summary();
            }
            return optimizer::common::Result<void>::Success();
        }
        auto served = state.ipcSession->ServeOne(
            [&state](const optimizer::ipc::IpcRequest& request,
                     optimizer::ipc::IpcReply& reply) {
                // 默认处理语义（复用会话凭据要求 expectedToken，与 ServeOne 缺省处理器一致）；
                // 另在受理侧记录解析摘要（R0 只读，不执行任何动作）。
                auto handled = optimizer::ipc::IpcSession::DefaultHandler(
                    request, reply, state.ipcExpectedToken);
                if (request.type ==
                    optimizer::ipc::IpcMessageType::FactsSnapshot) {
                    if (auto parsed =
                            optimizer::ipc::ParseFactsV1(request.payload);
                        parsed) {
                        state.ipcLastFactSummary =
                            optimizer::ipc::FormatFactsSummary(parsed.Value());
                        state.ipcLastFactCount = parsed.Value().size();
                        // ACT-006：宿主侧消费 Agent 用户活动观测（只读展示/编排种子；
                        // 未上报该键按 n/a，不伪装在场）。
                        state.ipcLastUserIdleSeconds =
                            optimizer::ipc::NumericFactValue(
                                parsed.Value(), "user_idle_seconds");
                        // IPC-019：前台窗口归属（数值键经白名单校验后读取）。
                        state.ipcLastForegroundPid =
                            optimizer::ipc::NumericFactValue(
                                parsed.Value(), "foreground_pid");
                    } else {
                        state.ipcLastFactSummary.clear();
                        state.ipcLastFactCount = 0;
                        state.ipcLastUserIdleSeconds.reset();
                        state.ipcLastForegroundPid.reset();
                    }
                }
                return handled;
            },
            std::chrono::milliseconds(900));
        if (!served) {
            const auto& error = served.ErrorValue();
            // 接受超时 = 暂无 Agent 连接：继续下一 tick；其余失败如实上报。
            if (error.domain == optimizer::common::ErrorDomain::Win32 &&
                error.code == ERROR_TIMEOUT) {
                return optimizer::common::Result<void>::Success();
            }
            // 身份/凭据拒绝（观察者已计数，可能已触发 Safe Mode）：非宿主故障，
            // 记录后继续下一 tick（IPC-010；此前此类拒绝会中止整个窗口）。
            if (state.ipcAuthRejected) {
                state.ipcAuthRejected = false;
                return optimizer::common::Result<void>::Success();
            }
            return optimizer::common::Result<void>::Failure(error);
        }
        const auto& result = served.Value();
        ++state.ipcClientsServed;
        state.ipcLastPayloadBytes = result.payloadBytes;
        state.ipcLastReplyAck =
            result.replyType == optimizer::ipc::IpcMessageType::Ack;
        std::wstring identity =
            L"pid " + std::to_wstring(result.clientPid) + L", session " +
            std::to_wstring(result.clientSessionId);
        if (!result.clientUserSid.empty()) {
            identity += L", user " + result.clientUserSid;
        }
        state.ipcLastIdentity = identity;
        std::wstring servedMessage =
            L"ipc  : client #" +
            std::to_wstring(state.ipcClientsServed) + L" from " + identity +
            (result.replyType == optimizer::ipc::IpcMessageType::Ack
                 ? L" (ack)"
                 : L" (error reply)");
        if (state.ipcLastFactCount > 0) {
            servedMessage +=
                L" facts=" + std::to_wstring(state.ipcLastFactCount);
            // ACT-006：逐客户端展示 Agent 上报的用户活动（该键只读展示，不做判定）。
            servedMessage += L", user idle=";
            if (state.ipcLastUserIdleSeconds) {
                servedMessage +=
                    std::to_wstring(*state.ipcLastUserIdleSeconds) + L" s";
            } else {
                servedMessage += L"n/a";
            }
            // SVC-006/007：记录进在场台账并标注分类（仅观测）；客户端键含会话号，
            // 区分同一 pid 在不同会话中的上报。
            if (state.ipcPresence) {
                const std::string clientKey =
                    std::to_string(result.clientPid) + ":" +
                    std::to_string(result.clientSessionId);
                const auto presence = state.ipcPresence->Record(
                    clientKey, state.ipcLastUserIdleSeconds);
                servedMessage += L" (" +
                    std::wstring(
                        optimizer::service::PresenceStateToString(presence)) +
                    L")";
            }
            // IPC-019：展示最近上报的前台窗口 pid（无前台/未上报按 none 不伪装）。
            servedMessage += L", fg pid=";
            if (state.ipcLastForegroundPid) {
                servedMessage += std::to_wstring(*state.ipcLastForegroundPid);
            } else {
                servedMessage += L"none";
            }
        }
        state.logger.Write(optimizer::logger::LogLevel::Info, L"service",
                           servedMessage);
    }
    // SVC-009：每 tick 末推进在场汇总（记录完本 tick 全部上报后触发变化事件）。
    if (state.ipcPresence) {
        (void)state.ipcPresence->Summary();
    }
    return optimizer::common::Result<void>::Success();
}

// 服务日志 sink：文件优先（服务模式无控制台），失败降级到 Debug 输出
// （Logger 契约：sink 失败不阻断、保持原 sink）。
void SetupServiceLogger(optimizer::logger::Logger& logger) noexcept {
    std::error_code dirError;
    const auto logDir = std::filesystem::temp_directory_path(dirError);
    if (dirError) {
        logger.SetDebugSink();
        return;
    }
    const auto logPath = logDir / L"CppOptimizerService.log";
    // SCM 服务读不到用户配置（服务侧配置目录属另一安全边界），故按与 [logging] 默认值
    // 一致的口径轮转：单文件 10 MiB、最多 5 个文件——长跑的服务日志不会无限增长。
    if (const auto file = logger.SetFileSink(logPath.wstring(),
                                             optimizer::logger::FileSinkOptions{});
        !file) {
        logger.SetDebugSink();
    }
}

// IPC 凭据/选项解析辅助（前向声明，定义见 IPC 命令区段；服务控制台命令复用同套解析）。
bool FindIpcToken(int argc, wchar_t* argv[], int start,
                  std::wstring& out) noexcept;
bool IsValidIpcToken(const std::wstring& token) noexcept;
std::optional<std::filesystem::path> FindIpcTokenFile(
    int argc, wchar_t* argv[], int start) noexcept;

// SVC-004：每用户默认宿主配置路径（%LOCALAPPDATA%\CppOptimizer\config.local.toml，
// 与恢复标记/凭据存储同目录）。LOCALAPPDATA 缺失时回退系统 Temp（避免空路径）。
std::filesystem::path DefaultHostConfigPath() noexcept {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = ::GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer, static_cast<DWORD>(MAX_PATH));
    std::filesystem::path root;
    if (len > 0 && len < static_cast<DWORD>(MAX_PATH)) {
        root = std::filesystem::path(buffer);
    } else {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec);
    }
    return root / L"CppOptimizer" / L"config.local.toml";
}

// SVC-010：在场转移时间线文件（每用户数据目录，与恢复标记同目录）。
std::filesystem::path DefaultPresenceTimelinePath() noexcept {
    return DefaultHostConfigPath().parent_path() / L"presence-timeline.log";
}

// AUD-002：审计记录持久化文件（同一每用户数据目录；仅在门禁开启且真实发生 R1 动作时创建）。
// 动作日记路径（仅本地存储）：与审计同目录，独立文件。
// 冷却台账路径（每用户文件，2026-09-20 口径）：与配置/审计同目录。
std::filesystem::path DefaultCooldownLedgerPath() noexcept {
    return DefaultHostConfigPath().parent_path() / L"gate-cooldowns.txt";
}

std::filesystem::path DefaultJournalPath() noexcept {
    return DefaultHostConfigPath().parent_path() / L"action-journal.log";
}

std::filesystem::path DefaultAuditLogPath() noexcept {
    return DefaultHostConfigPath().parent_path() / L"audit.log";
}

int RunServiceConsoleCommand(int argc, wchar_t* argv[]) {
    // --service console <s|run> [config.toml] [--ipc-facts] [--confirm-recovery] [--tray]：
    // 控制台托管——前台、Ctrl+C 优雅停止；<s> 为 1..60 秒有界窗口，run 表示常驻
    //（无时间上限，直到 Ctrl+C/关闭信号；异常退出由恢复标记在下一次启动检测）。可选
    // --ipc-facts 在运行期间同时作为受保护管道服务端常驻受理 Agent 事实（SVC-002/003，R0）。
    // 可选 [config.toml]（紧跟在秒数后的首个非选项参数）在 --ipc-facts 时经 [ipc] 节配置
    // Safe Mode 门禁窗口参数（IPC-014）；无显式配置时自动消费每用户默认配置
    //（%LOCALAPPDATA%\CppOptimizer\config.local.toml，SVC-004）。IPC-018：会话开始写恢复
    // 标记、正常结束清除；上次异常退出未确认时锁存 Safe Mode（暂停受理），
    // --confirm-recovery 显式确认后清除并继续（只读 R0 不受影响）。
    constexpr std::uint32_t kMaxSeconds = 60;
    bool resident = false;
    std::uint32_t seconds = 0;
    if (argc >= 4 && std::wstring_view(argv[3]) == L"run") {
        resident = true;
    } else if (argc < 4 || !ParseUint32(argv[3], seconds) || seconds == 0 ||
               seconds > kMaxSeconds) {
        ErrorLine{} << L"  --service console needs <seconds 1.." << kMaxSeconds
                   << L"|run>\n";
        return 2;
    }
    // 可选配置路径：argv[4] 为“非选项”token 时视为配置（其余位置参数均为选项及其值）。
    std::optional<std::filesystem::path> consoleConfig;
    int flagsStart = 4;
    if (argc > 4 && argv[4][0] != L'-') {
        consoleConfig = std::filesystem::path(argv[4]);
        flagsStart = 5;
    }
    bool ipcFacts = false;
    bool confirmRecovery = false;
    bool useTray = false; // SVC-005：--tray 通知区图标（右键“退出”停止宿主）
    for (int i = flagsStart; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--ipc-facts") {
            ipcFacts = true;
        } else if (arg == L"--confirm-recovery") {
            confirmRecovery = true;
        } else if (arg == L"--tray") {
            useTray = true;
        }
    }

    // IPC-014/SVC-004：Safe Mode 门禁窗口参数（供 banner/门禁构造引用，缺省 = 状态机默认）。
    optimizer::service::SafeModeGuard::Options safeModeOptions;
    std::optional<optimizer::config::ConfigSnapshot> consoleConfigSnapshot;
    std::filesystem::path hostConfigPath; // 实际消费的配置路径（空 = 未消费配置）
    std::wstring hostConfigError;         // 默认配置无效时的错误描述（供日志）
    bool hostConfigIsDefault = false;     // 消费的是每用户默认配置
    bool hostConfigInvalid = false;       // 默认配置存在但无效（Safe Mode 触发源）
    // [logging] 总是消费（宿主日志口径）；[ipc] 仅在 --ipc-facts 时消费。配置来源优先级：
    // 显式 [config.toml] -> 每用户默认配置（%LOCALAPPDATA%\CppOptimizer\config.local.toml，
    // 与恢复标记/凭据同目录，存在才消费）-> 内置默认（零回归）。显式配置无效 = 语义错误，
    // 启动期直接拒绝（错误配置显式暴露）；默认配置无效按环境异常处理 -> Safe Mode「配置无效」
    // 离散锁存（常驻宿主不因损坏的环境配置退出，R0 负载照常、暂停受理 Agent，见启动异常块）。
    if (consoleConfig) {
        hostConfigPath = consoleConfig.value();
    } else {
        std::error_code ec;
        const auto defaultPath = DefaultHostConfigPath();
        if (std::filesystem::exists(defaultPath, ec)) {
            hostConfigPath = defaultPath;
            hostConfigIsDefault = true;
        }
    }
    if (!hostConfigPath.empty()) {
        auto loaded = optimizer::config::LoadConfig(hostConfigPath.wstring());
        if (!loaded) {
            const auto& error = loaded.ErrorValue();
            hostConfigError =
                L"[" + std::wstring(optimizer::common::ToString(error.domain)) +
                L":" + std::to_wstring(error.code) + L"] " + error.message;
            if (hostConfigIsDefault) {
                hostConfigInvalid = true; // 锁存延后到门禁构造后（见启动异常块）
            } else {
                ErrorLine{} << L"  --service console config load failed "
                           << hostConfigError << L"\n";
                return 2;
            }
        } else {
            consoleConfigSnapshot = loaded.Value();
            if (ipcFacts) {
                // [ipc] 门禁参数（无 --ipc-facts 时受理门禁不存在，配置无意义）。
                const auto& safeMode = loaded.Value().ipc.safeMode;
                safeModeOptions.enabled = safeMode.enabled;
                safeModeOptions.failuresToEnter =
                    static_cast<std::size_t>(safeMode.failuresToEnter);
                safeModeOptions.countingWindow = std::chrono::milliseconds(
                    safeMode.countingWindowMs);
                safeModeOptions.cooldown =
                    std::chrono::milliseconds(safeMode.cooldownMs);
            }
        }
    }

    // SVC-007：在场阈值与 ACT-004 政策“用户在场”口径一致——[policy].user_away_idle_seconds
    // > 0（配置显式/默认给定时）采用其值，否则回退默认 15 秒（policy 0 = 政策不启用门禁，
    // 展示侧仍需非零阈值）。仅 --ipc-facts 的在场台账使用。
    std::uint32_t presenceAwaySeconds = kHostPresenceAwaySeconds;
    if (consoleConfigSnapshot) {
        presenceAwaySeconds = optimizer::service::EffectivePresenceAwaySeconds(
            consoleConfigSnapshot->policy.userAwayIdleSeconds,
            kHostPresenceAwaySeconds);
    }

    // 宿主单实例：只有 Agent 受理宿主需要——避免两个形态同时注册后登录拉起多个宿主，
    // 造成同一管道名与同一批每用户文件（配置/恢复标记/在场时间线/审计）被两个进程争抢
    // （包括恢复标记互相清除 -> 假的“上次异常退出未确认”）。
    // 在写恢复标记之前获取：本路径提前返回时不会留下误导性的恢复标记。
    std::shared_ptr<optimizer::service::HostInstanceLock> hostInstance;
    if (ipcFacts) {
        hostInstance = optimizer::service::TryAcquireHostInstance(
            optimizer::service::DefaultHostInstanceName());
        if (!hostInstance) {
            const std::wstring lockName =
                optimizer::service::DefaultHostInstanceName();
            ErrorLine{} << L"  another agent-intake host is already running (lock: "
                        << lockName << L")\n";
            ErrorLine{} << L"  hint     : only one host may serve the intake pipe and the\n"
                           L"             per-user state files; stop the other host first\n";
            return 2;
        }
    }

    ServiceHostDemoState state;
    // LOG-004：宿主日志消费 [logging]——目录（空 = 每用户默认目录）、级别、单文件上限、
    // 历史文件数、控制台副本开关。未消费配置时按 [logging] 默认口径（info / 10 MiB x 5 / 控制台开）。
    std::filesystem::path hostLogPath;
    {
        optimizer::config::LoggingConfig loggingConfig;
        bool configConsumed = false;
        if (consoleConfigSnapshot) {
            loggingConfig = consoleConfigSnapshot->logging;
            configConsumed = true;
        }
        std::filesystem::path logDir;
        if (!loggingConfig.directory.empty()) {
            logDir = std::filesystem::path(loggingConfig.directory);
        } else {
            logDir = DefaultHostConfigPath().parent_path();
        }
        hostLogPath = logDir / L"host.log";
        // 级别：非法级别不装作为已知（回退 info 并如实告知），与“危险开关失败安全”同口径。
        const auto level =
            optimizer::logger::LevelFromString(
                std::wstring(loggingConfig.level.begin(),
                             loggingConfig.level.end()));
        if (level) {
            state.logger.SetLevel(level.Value());
        }
        optimizer::logger::FileSinkOptions fileOptions;
        fileOptions.maxFileMb = loggingConfig.maxFileMb;
        fileOptions.maxFiles = loggingConfig.maxFiles;
        fileOptions.alsoConsole = loggingConfig.console;
        // LOG-008（修正）：程序日志**恒定异步**——Write 不阻塞业务线程（不干扰正常功能）；
        // 完整性由「Flush 同步保证 + 退出/析构排空」提供，而不是把写入变成同步。
        const auto file = state.logger.SetAsyncFileSink(
            hostLogPath.wstring(), fileOptions, 1024);
        if (!file) {
            // 日志文件不可用：降级到控制台（与既有 sink 降级契约一致），不伪装成功。
            state.logger.SetStderrSink();
            optimizer::common::WriteConsoleLine(
                L"  logging  : file sink unavailable, console only");
        }
        std::wostringstream loggingLine;
        loggingLine << L"  logging  : level "
                    << optimizer::logger::LevelToString(
                           level ? level.Value()
                                 : optimizer::logger::LogLevel::Info)
                    << L", file " << hostLogPath.wstring() << L" ("
                    << loggingConfig.maxFileMb << L" MB x "
                    << loggingConfig.maxFiles << L" backup), console "
                    << (loggingConfig.console ? L"on" : L"off")
                    << L", async on (flush-synced, drained on exit)"
                    << (configConsumed ? L"" : L" [defaults: no config consumed]");
        if (!level) {
            loggingLine << L" [level '"
                        << std::wstring(loggingConfig.level.begin(),
                                        loggingConfig.level.end())
                        << L"' unknown -> info]";
        }
        optimizer::common::WriteConsoleLine(loggingLine.str());
        if (hostConfigInvalid) {
            // 默认配置无效且本模式不受 Safe Mode 门禁影响（无 --ipc-facts）：如实告知使用默认口径。
            optimizer::common::WriteConsoleLine(
                L"  logging  : default config is invalid -> [logging] defaults used: " +
                hostConfigError);
        }
    }
    // IPC-018：恢复标记路径（会话开始写、正常结束清；异常退出留存供下次检测）。
    const auto recoveryMarkerPath =
        optimizer::service::DefaultRecoveryMarkerPath();
    if (ipcFacts) {
        state.ipcEnabled = true;
        state.ipcPipeName =
            L"\\\\.\\pipe\\CppOptimizerIpc"; // 与 --ipc-pipe 默认管道同名
        optimizer::ipc::IpcSession::Options ipcOptions;
        ipcOptions.persistentAccept = true; // 常驻监听：窗口内连续受理多客户端
        // 会话凭据/用户 SID 授权（IPC-005/006/007，与 --ipc-pipe server 同语义；
        // 缺省不要求，仅当显式配置才启用）。
        std::wstring ipcToken;
        if (FindIpcToken(argc, argv, flagsStart, ipcToken)) {
            if (!IsValidIpcToken(ipcToken)) {
                ErrorLine{}
                    << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
                return 2;
            }
            ipcOptions.expectedToken = ipcToken;
        }
        if (auto tokenFile = FindIpcTokenFile(argc, argv, flagsStart)) {
            auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
            if (!stored) {
                ErrorLine{} << L"  --ipc-token-file unreadable; run "
                              L"--ipc-credential provision first\n";
                return 2;
            }
            ipcOptions.expectedToken = stored.Value();
        }
        for (int i = flagsStart; i + 1 < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--ipc-allow-user") {
                const std::wstring sid = argv[i + 1];
                if (sid.empty() || sid.size() > 192) {
                    ErrorLine{}
                        << L"  --ipc-allow-user needs a valid SID string\n";
                    return 2;
                }
                ipcOptions.allowedClientSids.push_back(sid);
            }
        }
        state.ipcExpectedToken = ipcOptions.expectedToken; // 供 tick 自定义处理器复用
        // Safe Mode 门禁（IPC-010/013）：时间窗口内身份/凭据失败达阈值暂停受理新 Agent，
        // 冷却到期自动恢复；参数取 [ipc] 配置（IPC-014），无配置时用状态机默认
        // （阈值 3、窗口 5s、冷却 2s、启用）——缺省行为零回归。仅影响 Agent 受理。
        state.ipcSafeMode.emplace(safeModeOptions);
        // SVC-006/007：宿主在场台账（Agent user_idle 汇总；不参与 Safe Mode 触发判定）。
        optimizer::service::HostPresenceTracker::Options presenceOptions;
        presenceOptions.awayAfterSeconds = presenceAwaySeconds;
        state.ipcPresence.emplace(presenceOptions);
        // SVC-009：在场汇总变化事件（时间线日志 + 计数；只作观测/记录）。
        state.ipcPresence->SetOnChange(
            [&state](optimizer::service::PresenceState from,
                     optimizer::service::PresenceState to) {
                ++state.presenceTransitions;
                state.logger.Write(
                    optimizer::logger::LogLevel::Info, L"service",
                    L"presence : " +
                        std::wstring(
                            optimizer::service::PresenceStateToString(from)) +
                        L" -> " +
                        std::wstring(
                            optimizer::service::PresenceStateToString(to)));
                // SVC-010：转移同时追加到时间线文件（尽力而为，失败不阻断运行）。
                (void)optimizer::service::AppendPresenceTransitionLine(
                    DefaultPresenceTimelinePath(), from, to);
            });
        ipcOptions.verdictObserver =
            [&state](optimizer::ipc::IpcClientVerdict verdict) {
                auto& guard = *state.ipcSafeMode;
                const auto before = guard.State();
                if (verdict == optimizer::ipc::IpcClientVerdict::Accepted) {
                    return; // 正常受理不参与计数（窗口自然过期，IPC-013 语义）
                }
                guard.OnClientRejected();
                ++state.ipcRejectedClients;
                if (before == optimizer::service::SafeModeState::Normal &&
                    guard.State() ==
                        optimizer::service::SafeModeState::SafeMode) {
                    ++state.ipcSafeModeEntries;
                    state.logger.Write(
                        optimizer::logger::LogLevel::Info, L"service",
                        L"ipc  : Safe Mode entered - 暂停受理新 Agent（身份/凭据失败达阈值）");
                }
                state.ipcAuthRejected = true;
            };
        state.ipcSession = std::make_shared<optimizer::ipc::IpcSession>(
            optimizer::ipc::CreateWin32ServerBackend(state.ipcPipeName),
            ipcOptions);
        // IPC-015/016：启动环境离散异常（触发项：Native capability 探测异常、
        // 不支持的 OS/build）。受理 Agent 前做一次只读启动基线检查：Native 探测失败或核心只读
        // 能力缺失（ntdll 加载失败 / NtQuerySystemInformation / 状态码转换不可用），或操作系统
        // 不受支持（非 Win10/11 x64，RtlGetVersion + GetNativeSystemInfo 只读判定）——任一
        // 异常即离散异常锁存 Safe Mode，整个窗口暂停受理新 Agent（R0 负载/日志照常）。
        const auto nativeProbe =
            optimizer::platform::NativeApi::Instance().Probe();
        bool nativeAnomaly = false;
        if (!nativeProbe) {
            nativeAnomaly = true;
        } else {
            const auto& capabilities = nativeProbe.Value();
            nativeAnomaly =
                !capabilities.ntdllLoaded ||
                !capabilities.querySystemInformation ||
                !capabilities.ntStatusConversion;
        }
        bool osSupportAnomaly = false;
        if (auto osVersion =
                optimizer::platform::QueryOsVersion(); osVersion) {
            osSupportAnomaly =
                optimizer::platform::ClassifyOsSupport(
                    osVersion.Value(),
                    optimizer::platform::IsNativeX64()) !=
                optimizer::platform::OsSupport::Supported;
        } else {
            osSupportAnomaly = true; // 版本查询失败：保守视为异常（不伪装支持）
        }
        const bool startupAnomaly = nativeAnomaly || osSupportAnomaly;
        if (startupAnomaly) {
            state.ipcSafeMode->OnAnomalyDetected();
            ++state.ipcAnomalyEntries;
            if (nativeAnomaly) {
                state.ipcNativeProbeAnomaly = true;
                state.logger.Write(
                    optimizer::logger::LogLevel::Info, L"service",
                    L"ipc  : Safe Mode entered - Native capability 探测异常"
                    L"（R0 baseline 缺失）");
            }
            if (osSupportAnomaly) {
                state.ipcOsSupportAnomaly = true;
                state.logger.Write(
                    optimizer::logger::LogLevel::Info, L"service",
                    L"ipc  : Safe Mode entered - 操作系统不受支持"
                    L"（需 Windows 10/11 x64）");
            }
            state.logger.Write(optimizer::logger::LogLevel::Info, L"service",
                               L"ipc  : 暂停受理新 Agent（启动环境异常锁存）");
        } else {
            state.ipcNativeProbeOk = true;
        }
        // SVC-004：每用户默认配置无效 -> Safe Mode「配置无效」离散触发。
        // 显式配置无效已在启动期拒绝（语义错误显式暴露）；默认配置损坏按环境异常处理：
        // 锁存 Safe Mode、暂停受理 Agent，R0 负载照常；修复或移除配置后重启恢复。
        if (hostConfigInvalid) {
            state.ipcSafeMode->OnAnomalyDetected();
            ++state.ipcAnomalyEntries;
            state.ipcConfigAnomaly = true;
            state.logger.Write(optimizer::logger::LogLevel::Info, L"service",
                               L"ipc  : Safe Mode entered - 每用户默认配置无效（" +
                                   hostConfigPath.wstring() + L"）");
            state.logger.Write(optimizer::logger::LogLevel::Info, L"service",
                               L"ipc  : config load failed " + hostConfigError);
            state.logger.Write(optimizer::logger::LogLevel::Info, L"service",
                               L"ipc  : 暂停受理新 Agent（配置无效锁存；修复或移除配置后重启）");
        }
        // IPC-018：上次异常退出且恢复未确认，属 Safe Mode 离散异常触发项。启动先查恢复标记：
        // 标记存在 = 上次会话未正常结束（崩溃/被杀/失败返回）。--confirm-recovery 显式确认
        // 先清除标记；否则视为离散异常锁存 Safe Mode（暂停受理，需下次确认后恢复）。随后写入
        // 本次会话标记，正常结束（RunConsole 成功返回）时清除；失败/被杀则留存供下次检测。
        if (confirmRecovery) {
            (void)optimizer::service::ClearRecoveryMarker(recoveryMarkerPath);
            state.logger.Write(
                optimizer::logger::LogLevel::Info, L"service",
                L"ipc  : recovery marker cleared (user confirmed)");
        }
        const auto markerSet = [&recoveryMarkerPath]() {
            if (auto set = optimizer::service::IsRecoveryMarkerSet(
                    recoveryMarkerPath);
                set) {
                return set.Value();
            }
            return false; // 读取 IO 失败按未设置（目录异常不阻断启动，见模块注释）
        }();
        // 消费 `[application].safe_mode_on_recovery_error`（口径：可见但不阻断）：
        // true（默认）= 锁存 Safe Mode；false = 不暂停受理，但**照常上报**异常退出事实。
        const bool recoveryLatchEnabled =
            !consoleConfigSnapshot ||
            consoleConfigSnapshot->application.safeModeOnRecoveryError;
        const auto recoveryAction =
            optimizer::service::DecideRecoveryAnomalyAction(
                markerSet, confirmRecovery, recoveryLatchEnabled);
        if (recoveryAction == optimizer::service::RecoveryAnomalyAction::Latch) {
            state.ipcRecoveryUnconfirmed = true;
            state.ipcSafeMode->OnAnomalyDetected();
            ++state.ipcAnomalyEntries;
            state.logger.Write(
                optimizer::logger::LogLevel::Info, L"service",
                L"ipc  : Safe Mode entered - 上次异常退出且恢复未确认"
                L"（pass --confirm-recovery to acknowledge）");
        } else if (recoveryAction ==
                   optimizer::service::RecoveryAnomalyAction::NoteOnly) {
            // 不阻断，但不得静默：如实上报“上次异常退出”且说明本次被配置放行。
            state.ipcRecoveryNotedOnly = true;
            state.logger.Write(
                optimizer::logger::LogLevel::Info, L"service",
                L"ipc  : last run exited abnormally (unconfirmed) - not blocked by "
                L"[application].safe_mode_on_recovery_error = false");
        }
        (void)optimizer::service::WriteRecoveryMarker(recoveryMarkerPath);
    }
    optimizer::service::ServiceHost::Options options;
    options.identity.name = kServiceName;
    options.identity.displayName = kServiceDisplayName;
    options.identity.description = kServiceDescription;
    optimizer::service::ServiceHost host(
        [&state] { return ServiceWorkloadTick(state); }, std::move(options),
        optimizer::service::CreateWin32ScmBackend());

    // SVC-005：--tray 时加通知区图标；右键“退出”回调请求宿主优雅停止。
    // SVC-008：--tray + --ipc-facts 时给托盘窗口挂事件驱动输入（Raw Input）——宿主本机
    // 输入也计入在场台账（不再只依赖 Agent 上报）。
    std::shared_ptr<optimizer::service::TrayHost> trayHost;
    if (useTray) {
        optimizer::service::TrayHost::Options trayOptions;
        if (ipcFacts) {
            state.rawInput =
                std::make_shared<optimizer::activity::RawInputEventSource>();
            state.lastInputFallback =
                optimizer::activity::CreateWin32LastInputBackend();
            // 观察者指针指向 state.rawInput：托盘线程先于其析构 join（本函数内安全）。
            trayOptions.messageObserver =
                [source = state.rawInput.get()](UINT message, WPARAM wParam,
                                                LPARAM lParam) -> bool {
                    return source->OnWindowMessage(message, wParam, lParam);
                };
        }
        trayHost = std::make_shared<optimizer::service::TrayHost>(trayOptions);
        auto started = trayHost->Start([&host] { host.RequestStop(); });
        if (!started) {
            const auto& error = started.ErrorValue();
            ErrorLine{} << L"  tray start failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            if (ipcFacts) {
                // 会话未真正开始：不留“异常退出”标记（避免误导下次启动）。
                (void)optimizer::service::ClearRecoveryMarker(
                    recoveryMarkerPath);
            }
            return 2;
        }
        // SVC-008：绑定到托盘隐藏窗口并注册键盘/鼠标原始输入（非前台也接收）。
        if (ipcFacts && state.rawInput) {
            auto attached =
                state.rawInput->Attach(trayHost->WindowHandle());
            state.rawInputAttached = attached.HasValue();
            if (!attached) {
                const auto& error = attached.ErrorValue();
                state.logger.Write(
                    optimizer::logger::LogLevel::Info, L"service",
                    L"input : raw sink attach failed, fallback to polling [" +
                        std::wstring(optimizer::common::ToString(
                            error.domain)) +
                        L":" + std::to_wstring(error.code) + L"] " +
                        error.message);
            }
        }
    }

    std::wostringstream header;
    if (resident) {
        header << L"Service host (console mode, R0 workload, resident until "
                  L"Ctrl+C)";
    } else {
        header << L"Service host (console mode, R0 workload, " << seconds
               << L" s)";
    }
    optimizer::common::WriteConsoleLine(header.str());
    optimizer::common::WriteConsoleLine(
        L"  workload : memory snapshot + info log (read-only)");
    if (ipcFacts) {
        optimizer::common::WriteConsoleLine(
            L"             + protected-pipe facts consume (continuous, R0)");
        optimizer::common::WriteConsoleLine(
            L"  ipc      : pipe \\\\.\\pipe\\CppOptimizerIpc");
        {
            // 门禁参数来自 SafeModeGuard::Options（缺省 = 状态机默认），banner 随实际值输出。
            auto spec = [](std::int64_t ms) {
                if (ms % 1000 == 0) {
                    return std::to_wstring(ms / 1000) + L" s";
                }
                return std::to_wstring(ms) + L" ms";
            };
            std::wostringstream gateLine;
            gateLine << L"             safe mode : intake gate ";
            if (!safeModeOptions.enabled) {
                gateLine << L"off ([ipc].safe_mode_enabled = false)";
            } else {
                gateLine << L"on (" << safeModeOptions.failuresToEnter
                         << L" auth failures in "
                         << spec(safeModeOptions.countingWindow.count())
                         << L" -> " << spec(safeModeOptions.cooldown.count())
                         << L" pause)";
            }
            optimizer::common::WriteConsoleLine(gateLine.str());
            if (hostConfigInvalid) {
                optimizer::common::WriteConsoleLine(
                    L"  config   : invalid default config -> Safe Mode (agent "
                    L"intake paused); fix or remove and restart");
                optimizer::common::WriteConsoleLine(
                    L"  config   : " + hostConfigPath.wstring());
                optimizer::common::WriteConsoleLine(L"  config   : load failed " +
                                                    hostConfigError);
            } else if (consoleConfigSnapshot) {
                optimizer::common::WriteConsoleLine(
                    L"  config   : " + hostConfigPath.wstring() +
                    (hostConfigIsDefault ? L" (default, [ipc] safe_mode)"
                                         : L" ([ipc] safe_mode)"));
            } else {
                optimizer::common::WriteConsoleLine(
                    L"  config   : none (Safe Mode gate defaults)");
            }
            optimizer::common::WriteConsoleLine(
                state.ipcNativeProbeAnomaly
                    ? L"  native   : probe anomaly -> Safe Mode (agent intake "
                      L"paused for window)"
                    : L"  native   : probe ok (R0 read-only baseline)");
            optimizer::common::WriteConsoleLine(
                state.ipcOsSupportAnomaly
                    ? L"  os       : unsupported (need Windows 10/11 x64) -> Safe "
                      L"Mode (agent intake paused for window)"
                    : L"  os       : windows 10/11 x64 supported");
            if (state.ipcRecoveryUnconfirmed) {
                optimizer::common::WriteConsoleLine(
                    L"  recovery : last run exited abnormally (unconfirmed) -> "
                    L"Safe Mode (intake paused); pass --confirm-recovery to "
                    L"acknowledge");
            } else if (state.ipcRecoveryNotedOnly) {
                optimizer::common::WriteConsoleLine(
                    L"  recovery : last run exited abnormally (unconfirmed) -> not "
                    L"blocked (safe_mode_on_recovery_error = false; the fact is still "
                    L"logged)");
            } else if (confirmRecovery) {
                optimizer::common::WriteConsoleLine(
                    L"  recovery : acknowledged (marker cleared)");
            }
        }
    }
    if (useTray) {
        optimizer::common::WriteConsoleLine(
            L"  tray     : icon in notification area (right-click to exit)");
    }
    optimizer::common::WriteConsoleLine(
        resident ? L"  stop     : Ctrl+C or console close"
                 : L"  stop     : Ctrl+C or timeout");
    const auto result =
        resident ? host.RunConsole(std::nullopt)
                 : host.RunConsole(std::chrono::seconds(seconds));
    if (trayHost) {
        trayHost->Stop(); // 托盘线程收尾（幂等；菜单退出路径线程已自退出）
    }
    if (state.ipcSession) {
        state.ipcSession->Close(); // 结束常驻监听会话（幂等）
    }
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wostringstream err;
        err << L"  service host failed ["
            << optimizer::common::ToString(error.domain) << L":"
            << error.code << L"] " << error.message;
        optimizer::common::WriteConsoleLine(err.str());
        // 会话未正常结束：保留恢复标记（供下次启动检测“上次异常退出”）。
        return 2;
    }
    // 正常结束（timeout / Ctrl+C 优雅停止）：清除本次会话恢复标记。
    if (ipcFacts) {
        (void)optimizer::service::ClearRecoveryMarker(recoveryMarkerPath);
    }
    std::wostringstream ticksLine;
    ticksLine << L"  ticks    : " << state.tickCount;
    optimizer::common::WriteConsoleLine(ticksLine.str());
    if (state.ipcEnabled) {
        std::wostringstream ipcLine;
        if (state.ipcClientsServed > 0) {
            ipcLine << L"  ipc      : served " << state.ipcClientsServed
                    << L" client(s); last from " << state.ipcLastIdentity
                    << L", payload " << state.ipcLastPayloadBytes
                    << L" bytes, " << state.ipcLastFactCount
                    << L" facts, reply "
                    << (state.ipcLastReplyAck ? L"ack" : L"error");
            if (state.ipcLastFactCount > 0) {
                ipcLine << L", user idle ";
                if (state.ipcLastUserIdleSeconds) {
                    ipcLine << *state.ipcLastUserIdleSeconds << L" s";
                } else {
                    ipcLine << L"n/a";
                }
                // IPC-019：最近受理客户端上报的前台窗口 pid（无前台/未上报 none）。
                ipcLine << L", fg pid ";
                if (state.ipcLastForegroundPid) {
                    ipcLine << *state.ipcLastForegroundPid;
                } else {
                    ipcLine << L"none";
                }
            }
        } else {
            ipcLine << L"  ipc      : no Agent connected within window";
        }
        optimizer::common::WriteConsoleLine(ipcLine.str());
        if (state.rawInputAttached) {
            optimizer::common::WriteConsoleLine(
                L"  input    : raw input sink attached (event-driven, "
                L"keyboard+mouse)");
        }
        if (state.ipcPresence) {
            const auto hostPresence = state.ipcPresence->Summary();
            const auto clients = state.ipcPresence->Clients();
            std::size_t presentCount = 0;
            std::size_t awayCount = 0;
            std::size_t unknownCount = 0;
            for (const auto& client : clients) {
                switch (client.state) {
                    case optimizer::service::PresenceState::Present:
                        ++presentCount;
                        break;
                    case optimizer::service::PresenceState::Away:
                        ++awayCount;
                        break;
                    case optimizer::service::PresenceState::Unknown:
                        ++unknownCount;
                        break;
                }
            }
            std::wostringstream presenceLine;
            presenceLine << L"  presence : "
                         << optimizer::service::PresenceStateToString(
                                hostPresence)
                         << L" (present " << presentCount << L" / away "
                         << awayCount << L" / unknown " << unknownCount
                         << L", clients " << clients.size()
                         << L", away threshold " << presenceAwaySeconds
                         << L" s idle, transitions "
                         << state.presenceTransitions << L")";
            optimizer::common::WriteConsoleLine(presenceLine.str());
            for (const auto& client : clients) {
                std::wostringstream clientLine;
                clientLine << L"             ["
                           << std::wstring(client.key.begin(),
                                           client.key.end())
                           << L"] "
                           << optimizer::service::PresenceStateToString(
                                  client.state);
                if (client.idleSeconds) {
                    clientLine << L", idle " << *client.idleSeconds << L" s";
                }
                optimizer::common::WriteConsoleLine(clientLine.str());
            }
        }
        if (state.presenceTransitions > 0) {
            std::wostringstream timelineLine;
            timelineLine << L"  timeline : " << state.presenceTransitions
                         << L" presence transition(s) appended to "
                         << DefaultPresenceTimelinePath().wstring();
            optimizer::common::WriteConsoleLine(timelineLine.str());
        }
        if (state.ipcSafeMode) {
            std::wostringstream safeLine;
            safeLine << L"  safe mode: entered " << state.ipcSafeModeEntries
                     << L" time(s), rejected clients "
                     << state.ipcRejectedClients;
            if (state.ipcNativeProbeAnomaly) {
                safeLine << L", native probe anomaly";
            }
            if (state.ipcOsSupportAnomaly) {
                safeLine << L", unsupported os/build";
            }
            if (state.ipcConfigAnomaly) {
                safeLine << L", invalid default config";
            }
            if (state.ipcRecoveryUnconfirmed) {
                safeLine << L", recovery unconfirmed";
            }
            optimizer::common::WriteConsoleLine(safeLine.str());
        }
    }
    // LOG-008：异步模式（用户显式开启）下，窗口结束前冲刷一次，使本窗口记录在读文件时可见。
    state.logger.Flush();
    std::wostringstream stoppedLine;
    stoppedLine << L"  stopped  : "
                << (host.IsStopRequested() ? L"user (Ctrl+C or tray exit)"
                                           : L"timeout");
    optimizer::common::WriteConsoleLine(stoppedLine.str());
    return 0;
}

int RunServiceAsServiceCommand() {
    // --service service（或 SCM 以服务名启动）：进入 SCM 分发循环，
    // 直到 SCM 发送停止/关机控制码。
    ServiceHostDemoState state;
    SetupServiceLogger(state.logger);
    optimizer::service::ServiceHost::Options options;
    options.identity.name = kServiceName;
    options.identity.displayName = kServiceDisplayName;
    options.identity.description = kServiceDescription;
    optimizer::service::ServiceHost host(
        [&state] { return ServiceWorkloadTick(state); }, std::move(options),
        optimizer::service::CreateWin32ScmBackend());
    const auto result = host.RunService();
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wostringstream err;
        err << L"  service run failed ["
            << optimizer::common::ToString(error.domain) << L":"
            << error.code << L"] " << error.message;
        if (error.domain == optimizer::common::ErrorDomain::Win32 &&
            error.code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            err << L" (服务模式必须由 SCM 启动；普通命令行请用 --service console)";
        }
        optimizer::common::WriteConsoleLine(err.str());
        // 服务模式无控制台：同步留一份诊断到调试输出。
        ::OutputDebugStringW(err.str().c_str());
        return 2;
    }
    return 0;
}

int RunServiceInstallCommand(int argc, wchar_t* argv[]) {
    // --service install [exe-path]：注册服务（需要管理员；demand start 保守默认）。
    optimizer::service::ServiceIdentity identity;
    identity.name = kServiceName;
    identity.displayName = kServiceDisplayName;
    identity.description = kServiceDescription;
    if (argc >= 4) {
        identity.executablePath = argv[3];
    }
    JournalAction("agent.form_install", "service", true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: install SCM service (needs admin)");
    const auto result = optimizer::service::InstallService(identity);
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wostringstream err;
        err << L"  service install failed ["
            << optimizer::common::ToString(error.domain) << L":"
            << error.code << L"] " << error.message;
        if (error.domain == optimizer::common::ErrorDomain::Win32 &&
            error.code == ERROR_ACCESS_DENIED) {
            err << L" (需要管理员权限)";
        } else if (error.domain == optimizer::common::ErrorDomain::Win32 &&
                   error.code == ERROR_SERVICE_EXISTS) {
            err << L" (服务已存在，请先卸载)";
        }
        optimizer::common::WriteConsoleLine(err.str());
        return 2;
    }
    AuditAgentFormAction(optimizer::service::AgentForm::Service, true, true);
    std::wostringstream done;
    done << L"Service installed: " << kServiceName
         << L" (demand start, requires SCM start)";
    optimizer::common::WriteConsoleLine(done.str());
    optimizer::common::WriteConsoleLine(L"  start : sc start CppOptimizerService");
    optimizer::common::WriteConsoleLine(
        L"  test  : CppOptimizer.exe --service console 10");
    return 0;
}

int RunServiceUninstallCommand() {
    // --service uninstall：移除服务（需要管理员）。
    JournalAction("agent.form_remove", "service", true,
                  optimizer::audit::JournalPhase::Before,
                  "intent: uninstall SCM service (needs admin)");
    const auto result = optimizer::service::UninstallService(kServiceName);
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wostringstream err;
        err << L"  service uninstall failed ["
            << optimizer::common::ToString(error.domain) << L":"
            << error.code << L"] " << error.message;
        if (error.domain == optimizer::common::ErrorDomain::Win32 &&
            error.code == ERROR_ACCESS_DENIED) {
            err << L" (需要管理员权限)";
        } else if (error.domain == optimizer::common::ErrorDomain::Win32 &&
                   error.code == ERROR_SERVICE_DOES_NOT_EXIST) {
            err << L" (服务未安装)";
        }
        optimizer::common::WriteConsoleLine(err.str());
        return 2;
    }
    AuditAgentFormAction(optimizer::service::AgentForm::Service, false, true);
    std::wostringstream done;
    done << L"Service uninstalled: " << kServiceName;
    optimizer::common::WriteConsoleLine(done.str());
    return 0;
}

int RunServiceStatusCommand() {
    // --service status：只读查询 SCM 服务安装状态（不需要管理员）。
    // 未安装不是错误；查询本身失败（如无权限）如实失败，不冒充“未安装”。
    const auto queried = optimizer::service::QueryService(kServiceName);
    if (!queried) {
        const auto& error = queried.ErrorValue();
        ErrorLine{} << L"  service status failed ["
                    << optimizer::common::ToString(error.domain) << L":"
                    << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    optimizer::common::WriteConsoleLine(
        L"Service (SCM; install and uninstall need admin)");
    {
        std::wostringstream line;
        line << L"  name     : " << kServiceName;
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (!queried.Value().installed) {
        optimizer::common::WriteConsoleLine(L"  state    : not installed");
        return 0;
    }
    {
        std::wostringstream line;
        line << L"  state    : installed ("
             << optimizer::service::StateToString(queried.Value().state)
             << L")";
        optimizer::common::WriteConsoleLine(line.str());
    }
    {
        std::wostringstream line;
        line << L"  start    : "
             << optimizer::service::StartTypeToString(
                    queried.Value().autoStart, queried.Value().startTypeKnown);
        optimizer::common::WriteConsoleLine(line.str());
    }
    return 0;
}

// 形态动作执行器：启动项/计划任务复用既有命令实现，服务用其专用命令。
int RunAgentFormAction(optimizer::service::AgentForm target, bool install) {
    if (target == optimizer::service::AgentForm::Service) {
        return install ? RunServiceInstallCommand(1, nullptr)
                       : RunServiceUninstallCommand();
    }
    std::wstring verb(install ? L"install" : L"remove");
    wchar_t* argv[4] = {const_cast<wchar_t*>(L"CppOptimizer.exe"),
                        const_cast<wchar_t*>(L"--agent-form"), verb.data(),
                        nullptr};
    return (target == optimizer::service::AgentForm::StartupTray
                ? &RunStartupCommand
                : &RunScheduledTaskCommand)(3, argv);
}

// 注册类动作审计：形态安装/卸载必须可审计（危险操作策略的提权分界第 6 条）。
// 审计不可用时如实提示，但不回滚已完成的系统动作（回滚属用户显式选择）。
// 危险/注册类动作审计（成功与失败都记录）：operationId 与策略执行器保持一致（power.*/priority.*/
// agent.form_*），便于按操作聚合；审计不可用时如实提示，但不改变调用方动作的成败语义。
// 字段一律用 ASCII（operationId/target/detail 均为本仓库内的固定文本或十进制编号）。
void JournalAction(const char* operationId, const std::string& target, bool ok,
                   optimizer::audit::JournalPhase phase,
                   const std::string& detail) {
    const auto appended = optimizer::audit::AppendJournalLine(
        DefaultJournalPath(), phase, operationId, ok, target, detail);
    if (!appended) {
        const auto& error = appended.ErrorValue();
        std::wostringstream line;
        line << L"  journal  : action not recorded ["
             << optimizer::common::ToString(error.domain) << L":"
             << error.code << L"] " << error.message;
        optimizer::common::WriteConsoleLine(line.str());
    }
}

// 形态实态快照（ASCII）：日记的“操作后状态”字段。
std::string DescribeAgentFormsSnapshot() {
    std::string snapshot = "startup_tray=";
    if (const auto queried = optimizer::service::QueryStartupEntry(); queried) {
        snapshot += queried.Value().empty() ? "no" : "yes";
    } else {
        snapshot += "unknown";
    }
    snapshot += " task=";
    if (const auto queried = optimizer::service::QueryScheduledTask(
            optimizer::service::kScheduledTaskName);
        queried) {
        snapshot += queried.Value().installed ? "yes" : "no";
    } else {
        snapshot += "unknown";
    }
    snapshot += " service=";
    if (const auto queried = optimizer::service::QueryService(kServiceName);
        queried) {
        snapshot += queried.Value().installed ? "yes" : "no";
    } else {
        snapshot += "unknown";
    }
    return snapshot;
}

void AuditAction(const char* operationId, optimizer::audit::RiskLevel risk,
                 const std::string& target, bool ok, const std::string& detail,
                 const std::string& postState) {
    optimizer::audit::AuditRecord record;
    record.operationId = operationId;
    record.risk = risk;
    record.caller = "cli";
    record.target = target;
    record.ok = ok;
    record.detail = detail;
    const auto appended =
        optimizer::audit::AppendAuditLine(DefaultAuditLogPath(), record);
    if (!appended) {
        const auto& error = appended.ErrorValue();
        std::wostringstream line;
        line << L"  audit    : action not recorded ["
             << optimizer::common::ToString(error.domain) << L":"
             << error.code << L"] " << error.message;
        optimizer::common::WriteConsoleLine(line.str());
    }
    // 三段日记的后两段：操作后（结果）+ 操作后状态（快照）。
    JournalAction(operationId, target, ok, optimizer::audit::JournalPhase::After,
                  detail);
    JournalAction(operationId, target, true,
                  optimizer::audit::JournalPhase::State,
                  postState.empty() ? DescribeAgentFormsSnapshot() : postState);
}

// 形态安装/卸载的审计（target = 形态名）。
void AuditAgentFormAction(optimizer::service::AgentForm form, bool install,
                          bool ok) {
    const std::wstring formName(optimizer::service::AgentFormToString(form));
    std::string narrow;
    narrow.reserve(formName.size());
    for (const wchar_t ch : formName) {
        narrow.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : 0x3F);
    }
    AuditAction(install ? "agent.form_install" : "agent.form_remove",
                optimizer::audit::RiskLevel::R1, narrow, ok,
                install ? "explicit action (install)" : "explicit action (remove)",
                DescribeAgentFormsSnapshot());
}

int RunAgentFormApply(int argc, wchar_t* argv[]) {
    // --agent-form apply [config.toml] [--dry-run]：**声明式收敛**（半自动）。
    // 读声明形态 -> 与实态比较 -> 若需变更则执行（先装声明形态、再卸其它形态）。
    // 注册仍属**显式动作**（本命令就是那次动作），仍受提权与回滚约束；不在启动路径自动执行。
    std::optional<std::filesystem::path> configPath;
    bool dryRun = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--dry-run") {
            dryRun = true;
        } else if (configPath.has_value()) {
            ErrorLine{} << L"  unknown --agent-form apply option: " << arg << L"\n";
            return 2;
        } else {
            configPath = std::filesystem::path(argv[i]);
        }
    }
    std::string declared = "startup_tray";
    if (configPath.has_value()) {
        const auto loaded = optimizer::config::LoadConfig(configPath->wstring());
        if (!loaded) {
            const auto& error = loaded.ErrorValue();
            ErrorLine{} << L"  config load failed ["
                        << optimizer::common::ToString(error.domain) << L":"
                        << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        declared = loaded.Value().agent.form;
    }
    const auto declaredForm = optimizer::service::ParseAgentForm(
        std::wstring(declared.begin(), declared.end()));
    if (!declaredForm) {
        ErrorLine{} << L"  config declares an unsupported form\n";
        return 2;
    }
    bool startupInstalled = false;
    bool taskInstalled = false;
    bool serviceInstalled = false;
    if (const auto queried = optimizer::service::QueryStartupEntry(); queried) {
        startupInstalled = !queried.Value().empty();
    }
    if (const auto queried = optimizer::service::QueryScheduledTask(
            optimizer::service::kScheduledTaskName);
        queried) {
        taskInstalled = queried.Value().installed;
    }
    if (const auto queried = optimizer::service::QueryService(kServiceName);
        queried) {
        serviceInstalled = queried.Value().installed;
    }
    const auto actions = optimizer::service::ResolveFormApplyActions(
        *declaredForm, startupInstalled, taskInstalled, serviceInstalled);
    {
        std::wostringstream line;
        line << L"  declared : " << optimizer::service::AgentFormToString(*declaredForm)
             << L" (from "
             << (configPath.has_value() ? configPath->wstring()
                                        : std::wstring(L"built-in default"))
             << L")";
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (actions.empty()) {
        optimizer::common::WriteConsoleLine(
            L"  result   : already converged (nothing to do)");
        return 0;
    }
    for (const auto& action : actions) {
        std::wostringstream line;
        line << (action.install ? L"  install  : " : L"  remove   : ")
             << optimizer::service::AgentFormToString(action.form);
        if (dryRun) {
            line << L" (dry-run: not executed)";
        }
        optimizer::common::WriteConsoleLine(line.str());
    }
    if (dryRun) {
        optimizer::common::WriteConsoleLine(
            L"  note     : dry-run only; no system change was made");
        return 0;
    }
    for (const auto& action : actions) {
        // 审计由叶子命令各自写入（--startup/--scheduled-task/--service），此处不重复记录。
        const int result = RunAgentFormAction(action.form, action.install);
        if (result != 0) {
            ErrorLine{} << L"  apply stopped: "
                        << (action.install ? L"install" : L"remove") << L" "
                        << optimizer::service::AgentFormToString(action.form)
                        << L" failed (already-applied actions are kept; rerun after\n"
                           L"             fixing the cause, or revert with --agent-form remove)\n";
            return result;
        }
    }
    optimizer::common::WriteConsoleLine(L"  result   : converged");
    return 0;
}

int InvokeFormAction(int (*command)(int, wchar_t**), const wchar_t* action) {
    std::wstring verb(action);
    wchar_t* argv[4] = {const_cast<wchar_t*>(L"CppOptimizer.exe"),
                        const_cast<wchar_t*>(L"--agent-form"), verb.data(),
                        nullptr};
    return command(3, argv);
}

int RunAgentFormCommand(int argc, wchar_t* argv[]) {
    // --agent-form <status|install|remove> [startup_tray|task|service]：常驻形态入口。
    // 语义：
    // - **默认形态不可更改**：唯一默认是 startup_tray；本命令的 install/remove 只是“本次动作”，
    //   不写配置（配置 [agent].form 取值开放属后续切片）；
    // - 三个形态各自保持原有权限语义：startup_tray 标准用户即可；task 注册需管理员
    //   （任务本身不提权）；service 安装需管理员；
    // - status 只读汇总：逐个形态查询，单个形态查询失败只影响它自己的行（如实标注）；
    // - 未指定形态一律拒绝，不隐式回退到默认形态去安装。
    if (argc < 3) {
        ErrorLine{} << L"  --agent-form requires status|install|remove\n";
        return 2;
    }
    const std::wstring_view action(argv[2]);
    if (action == L"apply") {
        return RunAgentFormApply(argc, argv);
    }
    bool startupInstalled = false;
    bool taskInstalled = false;
    bool serviceInstalled = false;
    if (action == L"status") {
        // 可选配置路径：显示“声明的形态”与实态的差异（配置只表达意图，不自动注册）。
        // 先加载后输出：配置无效时干净失败（exit 2），不产生一半输出。
        std::optional<std::string> declaredForm;
        if (argc >= 4) {
            const auto loaded = optimizer::config::LoadConfig(argv[3]);
            if (!loaded) {
                const auto& error = loaded.ErrorValue();
                ErrorLine{} << L"  config load failed ["
                            << optimizer::common::ToString(error.domain) << L":"
                            << error.code << L"] " << error.message << L"\n";
                return 2;
            }
            declaredForm = loaded.Value().agent.form;
        }
        optimizer::common::WriteConsoleLine(
            L"Agent forms (read-only; the default is startup_tray)");
        {
            const auto queried = optimizer::service::QueryStartupEntry();
            if (!queried) {
                ErrorLine{} << L"  startup_tray : query failed ["
                            << optimizer::common::ToString(
                                   queried.ErrorValue().domain)
                            << L":" << queried.ErrorValue().code << L"] "
                            << queried.ErrorValue().message << L"\n";
            } else {
                startupInstalled = !queried.Value().empty();
                std::wostringstream line;
                line << L"  startup_tray : ";
                if (!startupInstalled) {
                    line << L"not installed";
                } else {
                    line << L"installed -> " << queried.Value();
                }
                optimizer::common::WriteConsoleLine(line.str());
            }
        }
        {
            const auto queried = optimizer::service::QueryScheduledTask(
                optimizer::service::kScheduledTaskName);
            if (!queried) {
                ErrorLine{} << L"  task         : query failed ["
                            << optimizer::common::ToString(
                                   queried.ErrorValue().domain)
                            << L":" << queried.ErrorValue().code << L"] "
                            << queried.ErrorValue().message << L"\n";
            } else {
                taskInstalled = queried.Value().installed;
                std::wostringstream line;
                line << L"  task         : ";
                if (!taskInstalled) {
                    line << L"not installed";
                } else {
                    line << L"installed";
                    if (!queried.Value().trigger.empty()) {
                        line << L" (trigger " << queried.Value().trigger << L")";
                    }
                    if (!queried.Value().commandLine.empty()) {
                        line << L" -> " << queried.Value().commandLine;
                    }
                }
                optimizer::common::WriteConsoleLine(line.str());
            }
        }
        {
            const auto queried = optimizer::service::QueryService(kServiceName);
            if (!queried) {
                ErrorLine{} << L"  service      : query failed ["
                            << optimizer::common::ToString(
                                   queried.ErrorValue().domain)
                            << L":" << queried.ErrorValue().code << L"] "
                            << queried.ErrorValue().message << L"\n";
            } else {
                serviceInstalled = queried.Value().installed;
                std::wostringstream line;
                line << L"  service      : ";
                if (!serviceInstalled) {
                    line << L"not installed";
                } else {
                    line << L"installed ("
                         << optimizer::service::StateToString(queried.Value().state)
                         << L", start "
                         << optimizer::service::StartTypeToString(
                                queried.Value().autoStart,
                                queried.Value().startTypeKnown)
                         << L")";
                }
                optimizer::common::WriteConsoleLine(line.str());
            }
        }
        optimizer::common::WriteConsoleLine(
            L"  act          : CppOptimizer.exe --agent-form install|remove <startup_tray|task|service>");
        if (declaredForm) {
            // 声明与实态的差异：如实列出，并给出**显式**下一步命令（不代劳注册）。
            const std::wstring declared(declaredForm->begin(), declaredForm->end());
            optimizer::common::WriteConsoleLine(
                L"  declared     : " + declared +
                L" (from config; a declared form never installs itself)");
            const bool declaredInstalled =
                *declaredForm == "startup_tray"
                    ? startupInstalled
                    : (*declaredForm == "task" ? taskInstalled : serviceInstalled);
            if (!declaredInstalled) {
                optimizer::common::WriteConsoleLine(
                    L"  note         : the declared form is not installed yet; run: "
                    L"CppOptimizer.exe --agent-form install " + declared);
            }
        }
        return 0;
    }
    if (action != L"install" && action != L"remove") {
        ErrorLine{} << L"  --agent-form requires status|install|remove\n";
        return 2;
    }
    if (argc < 4) {
        ErrorLine{} << L"  --agent-form " << action
                    << L" needs a form: startup_tray|task|service\n";
        return 2;
    }
    // 可选 --replace：显式接受“替换现有形态”（默认拒绝，不静默卸载任何东西）。
    bool replace = false;
    for (int i = 4; i < argc; ++i) {
        const std::wstring_view extra(argv[i]);
        if (extra == L"--replace") {
            replace = true;
        } else {
            ErrorLine{} << L"  unknown --agent-form option: " << argv[i]
                        << L" (expected --replace)\n";
            return 2;
        }
    }
    const auto form = optimizer::service::ParseAgentForm(argv[3]);
    if (!form) {
        ErrorLine{} << L"  unknown agent form: " << argv[3]
                    << L" (expected startup_tray|task|service)\n";
        return 2;
    }
    const bool installing = action == L"install";
    // 形态动作执行器：启动项/计划任务复用既有命令实现，服务用其专用命令。
    const auto runAction = [](optimizer::service::AgentForm target,
                              bool install) -> int {
        if (target == optimizer::service::AgentForm::Service) {
            return install ? RunServiceInstallCommand(1, nullptr)
                           : RunServiceUninstallCommand();
        }
        return InvokeFormAction(
            target == optimizer::service::AgentForm::StartupTray
                ? &RunStartupCommand
                : &RunScheduledTaskCommand,
            install ? L"install" : L"remove");
    };
    if (!installing) {
        return runAction(*form, false); // 卸载不需要互斥检查
    }
    // 形态互斥（单一形态生效）：安装前查其它形态；已注册则**默认拒绝**，
    // 要求用户显式选择（先卸载，或加 --replace 在新安装成功后卸载它们）。
    const auto startupQuery = optimizer::service::QueryStartupEntry();
    if (startupQuery) {
        startupInstalled = !startupQuery.Value().empty();
    }
    const auto taskQuery = optimizer::service::QueryScheduledTask(
        optimizer::service::kScheduledTaskName);
    if (taskQuery) {
        taskInstalled = taskQuery.Value().installed;
    }
    const auto serviceQuery = optimizer::service::QueryService(kServiceName);
    if (serviceQuery) {
        serviceInstalled = serviceQuery.Value().installed;
    }
    const auto conflicts = optimizer::service::ConflictingAgentForms(
        *form, startupInstalled, taskInstalled, serviceInstalled);
    if (!conflicts.empty() && !replace) {
        std::wostringstream line;
        line << L"  another agent form is already installed:";
        for (const auto conflict : conflicts) {
            line << L' ' << optimizer::service::AgentFormToString(conflict);
        }
        ErrorLine{} << line.str() << L"\n";
        ErrorLine{} << L"  hint     : only one form may be active at a time - remove it first\n"
                       L"             (--agent-form remove <form>) or pass --replace to\n"
                       L"             switch after the new form is installed\n";
        return 2;
    }
    const int installed = runAction(*form, true);
    if (installed != 0) {
        return installed; // 新形态未装成功：不动旧形态（不制造“两边都没有”的窗口）
    }
    for (const auto conflict : conflicts) {
        // 先装后卸：任一时刻都至少有一个可用形态。
        const int removed = runAction(conflict, false);
        if (removed != 0) {
            ErrorLine{} << L"  note     : the previous form could not be removed; both may be\n"
                           L"             registered until it is removed manually\n";
            return removed;
        }
    }
    return 0;
}

// 命名管道基名：服务端与客户端必须使用同一名称（可选后缀区分实例）。
constexpr wchar_t kIpcPipeBase[] = L"\\\\.\\pipe\\CppOptimizerIpc";

// 组装管道名。后缀仅接受 ASCII 字母数字与 -_（非法后缀返回空串，拒绝生成）。
std::wstring IpcPipeName(std::wstring_view suffix) noexcept {
    std::wstring name = kIpcPipeBase;
    for (const wchar_t ch : suffix) {
        const bool allowed =
            (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
        if (!allowed) {
            return {};
        }
    }
    name.append(suffix);
    return name;
}

// 在位置参数中取第一个“非选项”参数作后缀（跳过 --ipc-token / --ipc-allow-user
// 及其值），使 [suffix] 与选项的顺序无关。
std::wstring FindIpcSuffix(int argc, wchar_t* argv[], int start) noexcept {
    std::wstring suffix;
    bool skipNextValue = false;
    for (int i = start; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        const bool isOptionFlag = arg == L"--ipc-token" ||
                                  arg == L"--ipc-token-file" ||
                                  arg == L"--ipc-allow-user" ||
                                  arg == L"--session" ||
                                  arg == L"--frames" ||
                                  arg == L"--instances" ||
                                  arg == L"--interval-ms" ||
                                  arg == L"--max-interval-ms" ||
                                  arg == L"--away-after-seconds" ||
                                  arg == L"--away-cap-ms";
        if (!skipNextValue && isOptionFlag) {
            skipNextValue = true;
            continue;
        }
        if (skipNextValue) {
            skipNextValue = false;
            continue;
        }
        if (suffix.empty()) {
            suffix = argv[i];
        }
    }
    return suffix;
}

// 在位置参数中检测 --session 开关（多帧会话演示）；跳过各带值选项及其值，
// 与 FindIpcSuffix 的解析一致（顺序无关）。--session 本身不带值，直接判定。
bool HasIpcSessionFlag(int argc, wchar_t* argv[], int start) noexcept {
    bool skipNextValue = false;
    for (int i = start; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        const bool optionWithValue = arg == L"--ipc-token" ||
                                     arg == L"--ipc-token-file" ||
                                     arg == L"--ipc-allow-user" ||
                                     arg == L"--frames" ||
                                     arg == L"--instances" ||
                                     arg == L"--interval-ms" ||
                                     arg == L"--max-interval-ms" ||
                                     arg == L"--away-after-seconds" ||
                                     arg == L"--away-cap-ms";
        if (!skipNextValue && optionWithValue) {
            skipNextValue = true;
            continue;
        }
        if (skipNextValue) {
            skipNextValue = false;
            continue;
        }
        if (arg == L"--session") {
            return true;
        }
    }
    return false;
}

// 读取 --frames <n>（多帧会话演示帧数，连接一次依次发送）；缺省 1（单帧往返）。
// 解析失败返回 0（调用方校验 1..16）。
std::uint32_t FindIpcFrames(int argc, wchar_t* argv[], int start) noexcept {
    for (int i = start; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--frames") {
            std::uint32_t frames = 0;
            if (!ParseUint32(argv[i + 1], frames)) {
                return 0;
            }
            return frames;
        }
    }
    return 1;
}

// 读取 --instances <n>（多实例并发受理 worker/管道实例数）；缺省 1（单实例）。
// 解析失败返回 0（调用方校验 1..8）。
std::uint32_t FindIpcInstances(int argc, wchar_t* argv[], int start) noexcept {
    for (int i = start; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--instances") {
            std::uint32_t instances = 0;
            if (!ParseUint32(argv[i + 1], instances)) {
                return 0;
            }
            return instances;
        }
    }
    return 1;
}

// 读取 --interval-ms <n>（Agent 上报周期毫秒）；缺省 1000。解析失败返回 0（调用方校验）。
std::uint32_t FindIpcIntervalMs(int argc, wchar_t* argv[], int start) noexcept {
    for (int i = start; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--interval-ms") {
            std::uint32_t interval = 0;
            if (!ParseUint32(argv[i + 1], interval)) {
                return 0;
            }
            return interval;
        }
    }
    return 1000;
}

// 读取 --max-interval-ms <n>（自适应节奏放大上限）；缺省 0（未指定，由命令层给默认）。
// 解析失败返回 0。
std::uint32_t FindIpcMaxIntervalMs(int argc, wchar_t* argv[], int start) noexcept {
    for (int i = start; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--max-interval-ms") {
            std::uint32_t value = 0;
            if (!ParseUint32(argv[i + 1], value)) {
                return 0;
            }
            return value;
        }
    }
    return 0;
}

// 扫描 --ipc-token <值>（IPC-005 demo：共享会话凭据）。返回是否存在；
// 值写入 out（调用方随后校验字符集/长度，且不打印明文）。
bool FindIpcToken(int argc, wchar_t* argv[], int start,
                  std::wstring& out) noexcept {
    for (int i = start; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--ipc-token") {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

// 凭据字符集约束：ASCII 字母/数字/_/-，1..64（与 ipc_facts schema 一致）。
bool IsValidIpcToken(const std::wstring& token) noexcept {
    if (token.empty() || token.size() > 64) {
        return false;
    }
    for (const wchar_t ch : token) {
        const bool alnum =
            (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9');
        if (!alnum && ch != L'_' && ch != L'-') {
            return false;
        }
    }
    return true;
}

// 解析 --ipc-token-file：无该标志返回 nullopt；有则返回存储路径（显式值或默认
// 用户私有路径，见 ipc_credentials.hpp）。值以下划线外的“-”开头视为非法省略。
std::optional<std::filesystem::path> FindIpcTokenFile(
    int argc, wchar_t* argv[], int start) noexcept {
    for (int i = start; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--ipc-token-file") {
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                return std::filesystem::path(argv[i + 1]);
            }
            return optimizer::ipc::DefaultAgentTokenFilePath();
        }
    }
    return std::nullopt;
}

int RunAgentCommand(int argc, wchar_t* argv[]) {
    // --agent run <s> [suffix] [--interval-ms <n>] [--ipc-token <t>]
    //   [--ipc-token-file [<path>]]：Agent 运行实体演示（IPC-011，前台有界、周期上报）。
    // 在 s 秒窗口内每 interval 毫秒采集一次真实内存观测并经受保护命名管道上报
    // FactsSnapshot（每次独立连接：请求-应答配对；宿主离线/Safe Mode 暂停等连接失败
    // 有界重试与退避，计入 connect failures 后继续；窗口到期返回汇总）。ACT-005：每周期
    // 另附用户活动观测 user_idle_seconds（GetLastInputInfo 只读；查询失败省略不伪装）。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (argc < 4 || !ParseUint32(argv[3], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --agent run seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }
    const std::wstring suffix = FindIpcSuffix(argc, argv, 4);
    const std::wstring pipeName = IpcPipeName(suffix);
    if (pipeName.empty()) {
        ErrorLine{} << L"  --agent suffix must be ASCII letters/digits/-/_\n";
        return 2;
    }
    const std::uint32_t intervalMs = FindIpcIntervalMs(argc, argv, 4);
    if (intervalMs == 0 || intervalMs < 50 || intervalMs > 10000) {
        ErrorLine{} << L"  --interval-ms must be in 50..10000\n";
        return 2;
    }
    // 自适应节奏放大上限（IPC-012）：显式须 >= interval；缺省 max(3*interval, 5s)。
    const std::uint32_t intervalCapMs = FindIpcMaxIntervalMs(argc, argv, 4);
    if (intervalCapMs != 0 &&
        (intervalCapMs < intervalMs || intervalCapMs > 60000)) {
        ErrorLine{} << L"  --max-interval-ms must be in [" << intervalMs
                   << L"..60000]\n";
        return 2;
    }
    const std::uint32_t effectiveCapMs =
        intervalCapMs != 0
            ? intervalCapMs
            : (intervalMs * 3 > 5000u ? intervalMs * 3 : 5000u);
    // ACT-007 在场感知节奏（可选）：--away-after-seconds <1..86400> 用户空闲达该秒数即放大
    // 上报间隔至 --away-cap-ms（缺省 = effectiveCapMs，与失败退避同封顶）；未配置 = 关闭。
    std::int64_t awayAfterSeconds = 0;
    std::int64_t awayStepSeconds = 1;
    std::uint32_t awayCapMs = 0;
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--away-after-seconds") {
            std::uint32_t value = 0;
            if (!ParseUint32(argv[i + 1], value) || value == 0 ||
                value > 86400) {
                ErrorLine{}
                    << L"  --away-after-seconds must be in 1..86400\n";
                return 2;
            }
            awayAfterSeconds = value;
        } else if (std::wstring_view(argv[i]) == L"--away-cap-ms") {
            std::uint32_t value = 0;
            if (!ParseUint32(argv[i + 1], value) || value < intervalMs ||
                value > 60000) {
                ErrorLine{} << L"  --away-cap-ms must be in [" << intervalMs
                           << L"..60000]\n";
                return 2;
            }
            awayCapMs = value;
        }
    }
    if (awayAfterSeconds > 0 && awayCapMs == 0) {
        awayCapMs = effectiveCapMs; // 缺省封顶 = 失败退避同款
    }
    std::wstring token;
    bool hasToken = false;
    if (FindIpcToken(argc, argv, 4, token)) {
        if (!IsValidIpcToken(token)) {
            ErrorLine{}
                << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
            return 2;
        }
        hasToken = true;
    }
    if (auto tokenFile = FindIpcTokenFile(argc, argv, 4)) {
        auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
        if (!stored) {
            ErrorLine{} << L"  --ipc-token-file unreadable; run "
                          L"--ipc-credential provision first\n";
            return 2;
        }
        token = stored.Value();
        hasToken = true;
    }

    // 每周期构造一帧真实内存事实（QueryMemoryStatus 只读；ACT-005 起另附用户活动观测
    // user_idle_seconds——GetLastInputInfo 只读，距最近键鼠输入秒数，锁屏/断开时钟冻结自然
    // 增长；查询失败省略该键不伪装）。可选 agent_token 凭据不回显明文。requestId 单调递增，
    // 重试沿用当次 id（与应答配对）。
    auto lastInputBackend =
        optimizer::activity::CreateWin32LastInputBackend();
    // IPC-019：前台窗口归属探测（GetForegroundWindow 只读）。
    auto foregroundProbe = optimizer::activity::CreateWin32ForegroundProbe();
    std::uint32_t nextRequestId = 0;
    const auto buildRequest =
        [&]() -> optimizer::common::Result<optimizer::ipc::IpcFrameRequest> {
        auto memoryStatus = optimizer::memory::QueryMemoryStatus();
        if (!memoryStatus) {
            return optimizer::common::Result<
                optimizer::ipc::IpcFrameRequest>::Failure(
                memoryStatus.ErrorValue());
        }
        const auto& memory = memoryStatus.Value();
        std::vector<optimizer::ipc::IpcFact> facts;
        facts.push_back(optimizer::ipc::IpcFact{
            "client_pid", std::to_string(::GetCurrentProcessId())});
        facts.push_back(optimizer::ipc::IpcFact{
            "memory_total_mb",
            std::to_string(memory.totalPhysicalBytes / (1024 * 1024))});
        facts.push_back(optimizer::ipc::IpcFact{
            "memory_available_mb",
            std::to_string(memory.availablePhysicalBytes / (1024 * 1024))});
        facts.push_back(optimizer::ipc::IpcFact{
            "memory_load_percent",
            std::to_string(memory.memoryLoadPercent)});
        // 用户活动观测（ACT-005）：查询成功才附带（不伪装）；失败省略该键。
        if (auto input = lastInputBackend->Query()) {
            const auto& sample = input.Value();
            const std::int64_t idleSeconds =
                optimizer::activity::IdleMilliseconds(
                    sample.nowTick, sample.lastInputTick) /
                1000;
            facts.push_back(optimizer::ipc::IpcFact{
                "user_idle_seconds", std::to_string(idleSeconds)});
        }
        // 前台窗口归属（IPC-019）：前台窗口存在才附 pid（无前台窗口/查询失败省略不伪装）。
        if (auto foreground = foregroundProbe->Query();
            foreground && foreground.Value().hasWindow) {
            facts.push_back(optimizer::ipc::IpcFact{
                "foreground_pid", std::to_string(foreground.Value().pid)});
        }
        facts.push_back(
            optimizer::ipc::IpcFact{"observer", "CppOptimizer agent demo"});
        if (hasToken) {
            std::string narrowToken;
            narrowToken.reserve(token.size());
            for (const wchar_t ch : token) {
                narrowToken.push_back(static_cast<char>(ch));
            }
            facts.push_back(optimizer::ipc::IpcFact{
                std::string(optimizer::ipc::kFactsTokenKey), narrowToken});
        }
        auto encoded = optimizer::ipc::SerializeFactsV1(facts);
        if (!encoded) {
            return optimizer::common::Result<
                optimizer::ipc::IpcFrameRequest>::Failure(
                encoded.ErrorValue());
        }
        optimizer::ipc::IpcFrameRequest request;
        request.type = optimizer::ipc::IpcMessageType::FactsSnapshot;
        request.requestId = ++nextRequestId;
        request.payload = std::move(encoded).Value();
        return optimizer::common::Result<
            optimizer::ipc::IpcFrameRequest>::Success(std::move(request));
    };

    optimizer::ipc::IpcPeriodicReportOptions reportOptions;
    reportOptions.pipePath = pipeName;
    reportOptions.window = std::chrono::milliseconds(seconds) * 1000;
    reportOptions.interval = std::chrono::milliseconds(intervalMs);
    reportOptions.intervalCap =
        std::chrono::milliseconds(effectiveCapMs); // 自适应放大上限
    reportOptions.ioTimeout = std::chrono::milliseconds(3000);
    reportOptions.maxConnectAttempts = 3;
    reportOptions.reconnectBackoff = std::chrono::milliseconds(300);
    // ACT-007 在场退避选项（0 = 关闭缺省，零回归）。
    reportOptions.userAwayAfterSeconds = awayAfterSeconds;
    reportOptions.userAwayStepSeconds = awayStepSeconds;
    reportOptions.userAwayCap = std::chrono::milliseconds(awayCapMs);
    // idle 采样回调：复用 activity 只读后端（查询失败按在场——测量缺失不节流）。
    std::function<optimizer::common::Result<std::int64_t>()> idleProvider;
    if (awayAfterSeconds > 0) {
        idleProvider =
            [backend = lastInputBackend]()
            -> optimizer::common::Result<std::int64_t> {
                auto query = backend->Query();
                if (!query) {
                    return optimizer::common::Result<std::int64_t>::Failure(
                        query.ErrorValue());
                }
                const auto& sample = query.Value();
                return optimizer::common::Result<std::int64_t>::Success(
                    optimizer::activity::IdleMilliseconds(
                        sample.nowTick, sample.lastInputTick) /
                    1000);
            };
    }

    std::wcout << L"Agent (periodic reporter, foreground bounded, " << seconds
               << L" s)\n";
    std::wcout << L"  pipe     : " << pipeName << L"\n";
    std::wcout << L"  report   : every " << intervalMs
               << L" ms (grow to " << effectiveCapMs
               << L" ms after repeated failures), up to 3 connect attempts per report\n";
    if (awayAfterSeconds > 0) {
        std::wcout << L"  away     : user idle >= " << awayAfterSeconds
                   << L" s -> grow to " << awayCapMs
                   << L" ms (presence-aware pacing, ACT-007; resume on input)\n";
    }
    std::wcout
        << L"  facts    : memory (total/available/load) + user_idle_seconds "
           L"(ACT-005, GetLastInputInfo read-only; omitted on query failure)"
        << L" + foreground_pid (IPC-019, foreground window owning pid; omitted\n"
        << L"             when none / on query failure)\n";
    if (hasToken) {
        std::wcout << L"  auth     : session token supplied (hidden)\n";
    }

    auto backend = optimizer::ipc::CreateWin32ClientBackend();
    auto result = optimizer::ipc::RunPeriodicReporter(backend, reportOptions,
                                                      buildRequest,
                                                      idleProvider);
    if (!result) {
        const auto& error = result.ErrorValue();
        ErrorLine{} << L"  agent run failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    const auto& summary = result.Value();
    std::wcout << L"  summary  : sent " << summary.reportsSent
               << L" report(s), connect failures " << summary.connectFailures
               << L"\n";
    if (summary.reportsSent == 0) {
        std::wcout << L"  -> host unreachable within window (offline / Safe Mode\n"
                      L"     pause / rejected); agent stays bounded and exits\n";
    } else {
        std::wcout << L"  last ack : " << (summary.lastReplyAck ? L"yes" : L"no")
                   << L"\n";
    }
    return 0;
}

int RunIpcCredentialCommand(int argc, wchar_t* argv[]) {
    // --ipc-credential provision [path] [--force] / status [path]（IPC-007 真实供给）。
    // token 永不打印；path 缺省用用户私有默认路径（LOCALAPPDATA\CppOptimizer）。
    const std::wstring_view action(argv[2]);
    std::filesystem::path path;
    bool havePath = false;
    bool force = false;
    for (int i = 3; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--force") {
            force = true;
            continue;
        }
        if (!havePath && argv[i][0] != L'-') {
            path = argv[i];
            havePath = true;
        }
    }
    if (!havePath) {
        path = optimizer::ipc::DefaultAgentTokenFilePath();
    }
    if (path.empty()) {
        ErrorLine{}
            << L"  cannot locate LOCALAPPDATA; provide an explicit <path>\n";
        return 2;
    }
    if (action == L"provision") {
        if (auto result = optimizer::ipc::ProvisionAgentTokenFile(path, force);
            !result) {
            const auto& error = result.ErrorValue();
            ErrorLine{} << L"  provision failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        optimizer::common::WriteConsoleLine(
            (force ? L"  token store rotated (hidden): "
                   : L"  token store provisioned (hidden): ") +
            path.wstring());
        return 0;
    }
    if (action == L"status") {
        if (optimizer::ipc::IsAgentTokenProvisioned(path)) {
            optimizer::common::WriteConsoleLine(
                L"  token store OK (provisioned, hidden): " + path.wstring());
        } else {
            optimizer::common::WriteConsoleLine(
                L"  token store NOT provisioned: " + path.wstring());
            optimizer::common::WriteConsoleLine(
                L"  -> run: CppOptimizer.exe --ipc-credential provision");
        }
        return 0;
    }
    ErrorLine{} << L"  --ipc-credential requires provision|status\n";
    return 2;
}

int RunIpcServerCommand(int argc, wchar_t* argv[]) {
    // --ipc-pipe server <s> [suffix] [--session] [--instances <1..8>]
    //   [--ipc-token <t>]：受保护命名管道服务端演示（IPC-005/008/009）。前台、有界
    // （s 秒）：等待客户端连接，逐帧严格校验（未知版本/类型/超长载荷 -> Error 应答），
    // 默认处理器应答（Ping->Ack；FactsSnapshot 依次过 CPOPFACTS/1 语法解析与 v1 键语义
    // 白名单，合法回 Ack 摘要、任一违反回 Error(InvalidFacts)；配置 --ipc-token 后还要求
    // 载荷携带匹配的 agent_token 事实，缺失/不匹配回 Error(AuthFailed)），输出客户端
    // 身份（PID + 会话）与请求/应答摘要。缺省每客户端一帧；--session 在同一连接上连续
    // 服务多帧（客户端关闭/帧间空闲/窗口到期结束，IPC-008）；--instances n（>1）以 n 个
    // 独立实例/线程并发受理并服务多个客户端（每连接按会话语义，IPC-009）。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (argc < 4 || !ParseUint32(argv[3], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        ErrorLine{} << L"  --ipc-pipe server seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }
    const std::wstring suffix = FindIpcSuffix(argc, argv, 4);
    const std::wstring pipeName = IpcPipeName(suffix);
    if (pipeName.empty()) {
        ErrorLine{} << L"  --ipc-pipe suffix must be ASCII letters/digits/-/_\n";
        return 2;
    }

    optimizer::ipc::IpcSession::Options sessionOptions;
    std::wstring token;
    if (FindIpcToken(argc, argv, 4, token)) {
        if (!IsValidIpcToken(token)) {
            ErrorLine{}
                << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
            return 2;
        }
        sessionOptions.expectedToken = token; // 明文仅限 demo；真实供给属后续切片
    }
    // IPC-007 真实供给：--ipc-token-file 优先（私有 ACL 存储），替代/覆盖内联明文。
    if (auto tokenFile = FindIpcTokenFile(argc, argv, 4)) {
        auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
        if (!stored) {
            ErrorLine{} << L"  --ipc-token-file unreadable; run "
                          L"--ipc-credential provision first\n";
            return 2;
        }
        sessionOptions.expectedToken = stored.Value();
    }
    // 客户端用户 SID 授权白名单（IPC-006，访问令牌只读查询）。
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--ipc-allow-user") {
            const std::wstring sid = argv[i + 1];
            if (sid.empty() || sid.size() > 192) {
                ErrorLine{} << L"  --ipc-allow-user needs a valid SID string\n";
                return 2;
            }
            sessionOptions.allowedClientSids.push_back(sid);
        }
    }
    // 多帧会话（IPC-008）：--session 接受一个客户端后在同一连接上连续服务多帧
    //（帧间空闲/窗口到期结束），连接内不再每帧重连；缺省保持单帧语义不变。
    const bool sessionMode = HasIpcSessionFlag(argc, argv, 4);
    const std::uint32_t instances = FindIpcInstances(argc, argv, 4);
    if (instances == 0 || instances > 8) {
        ErrorLine{} << L"  --instances must be 1..8\n";
        return 2;
    }
    const bool concurrentMode = instances > 1; // 多实例并发受理（IPC-009）
    auto backend = optimizer::ipc::CreateWin32ServerBackend(pipeName);
    optimizer::ipc::IpcSession session(backend, sessionOptions);

    std::wcout << L"IPC pipe server (protected transport, ";
    if (concurrentMode) {
        std::wcout << instances << L" concurrent instances, ";
    } else if (sessionMode) {
        std::wcout << L"multi-frame session, ";
    } else {
        std::wcout << L"single client, ";
    }
    std::wcout << seconds << L" s)\n";
    std::wcout << L"  pipe     : " << pipeName << L"\n";
    if (!sessionOptions.expectedToken.empty()) {
        std::wcout << L"  auth     : session token required (hidden)\n";
    }
    if (!sessionOptions.allowedClientSids.empty()) {
        std::wcout << L"  auth     : client user SID allow-list: "
                   << sessionOptions.allowedClientSids.front() << L"\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    // 帧间空闲上限（心跳节奏）：客户端超过该时长未发帧视为心跳丢失，会话结束。
    const std::chrono::milliseconds kSessionIdle(2000);

    // 多实例并发（IPC-009）：N 个独立实例/线程并发受理并服务客户端，每连接按多帧
    // 会话语义服务；窗口到期 join 全部 worker 并释放实例（无脱逸线程）。接受超时/
    // 无客户端连接属正常结束（非失败）。
    if (concurrentMode) {
        optimizer::ipc::IpcSession::Options workerOptions = sessionOptions;
        workerOptions.persistentAccept = true; // worker 实例跨会话保持监听（无空窗）
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        auto result = optimizer::ipc::RunConcurrentServer(
            instances, remaining, kSessionIdle, nullptr,
            [pipeName, workerOptions]() {
                return std::make_shared<optimizer::ipc::IpcSession>(
                    optimizer::ipc::CreateWin32ServerBackend(pipeName),
                    workerOptions);
            });
        if (!result) {
            const auto& error = result.ErrorValue();
            ErrorLine{} << L"  concurrent serve failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        const auto& summary = result.Value();
        std::wcout << L"  concurrent: " << summary.workers
                   << L" worker(s), served " << summary.clientsServed
                   << L" client session(s), failed " << summary.failedSessions
                   << L"\n";
        if (summary.clientsServed == 0) {
            std::wcout << L"  no client connected within " << seconds
                       << L" s (bounded window ended)\n";
            return 0;
        }
        for (std::size_t i = 0; i < summary.sessions.size(); ++i) {
            const auto& item = summary.sessions[i];
            std::wcout << L"  client #" << (i + 1) << L" : pid "
                       << item.clientPid << L", session "
                       << item.clientSessionId << L", served "
                       << item.framesServed << L" frame(s), end: "
                       << optimizer::ipc::IpcSessionEndReasonToString(
                              item.endReason)
                       << L"\n";
        }
        return 0;
    }

    // 单实例路径：默认单帧 / --session 同一连接多帧（既有语义不变）。
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const optimizer::common::Result<optimizer::ipc::IpcServeResult> served =
        sessionMode
            ? session.ServeSession(nullptr, remaining, kSessionIdle,
                                   remaining + kSessionIdle)
            : session.ServeOne(nullptr, remaining);
    if (!served) {
        const auto& error = served.ErrorValue();
        if (error.domain == optimizer::common::ErrorDomain::Win32 &&
            error.code == ERROR_TIMEOUT) {
            // 有界窗口内无客户端连接/会话无帧：正常结束（非失败）。
            std::wcout << L"  no client connected within " << seconds
                       << L" s (bounded window ended)\n";
            return 0;
        }
        ErrorLine{} << L"  serve failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& result = served.Value();
    std::wcout << L"  client   : pid " << result.clientPid << L", session "
               << result.clientSessionId << L"\n";
    if (!result.clientUserSid.empty()) {
        std::wcout << L"  identity : user SID " << result.clientUserSid.c_str()
                   << L"\n";
    }
    if (sessionMode) {
        std::wcout << L"  session  : served " << result.framesServed
                   << L" frame(s) on one connection, end: "
                   << optimizer::ipc::IpcSessionEndReasonToString(
                          result.endReason)
                   << L"\n";
    }
    std::wcout << L"  request  : "
               << optimizer::ipc::MessageTypeToString(result.requestType)
               << L" id=" << result.requestId << L" payload="
               << result.payloadBytes << L" bytes\n";
    if (result.replyType == optimizer::ipc::IpcMessageType::Error) {
        // Error 回执可能来自 Facts 契约/白名单违反或凭据校验失败：原因在客户端输出。
        std::wcout << L"  reply    : error sent (request rejected; see client)\n";
    } else if (result.requestType ==
               optimizer::ipc::IpcMessageType::FactsSnapshot) {
        std::wcout << L"  reply    : ack sent (facts parsed and summarized)\n";
    } else {
        std::wcout << L"  reply    : ack sent (frame validated)\n";
    }
    return 0;
}

int RunIpcClientCommand(int argc, wchar_t* argv[]) {
    // --ipc-pipe client [suffix] [--ipc-token <t>] [--ipc-token-file [<path>]]
    //   [--frames <n>]：受保护命名管道客户端演示（IPC-005/008）。缺省：连接服务端
    // 发送一帧 FactsSnapshot（CPOPFACTS/1 结构化事实：真实内存观测 + 自报身份；
    // 配置 --ipc-token/--ipc-token-file 时附 agent_token 凭据事实），读取并校验
    // 应答帧（requestId 配对、Error 应答不伪装成功）。--frames n（2..16）：复用
    // 同一连接依次发送 n 帧（连接内多帧会话，逐帧独立内存观测，不再逐帧重连）。
    const std::wstring suffix = FindIpcSuffix(argc, argv, 3);
    const std::wstring pipeName = IpcPipeName(suffix);
    if (pipeName.empty()) {
        ErrorLine{} << L"  --ipc-pipe suffix must be ASCII letters/digits/-/_\n";
        return 2;
    }
    std::wstring token;
    bool hasToken = false;
    if (FindIpcToken(argc, argv, 3, token)) {
        if (!IsValidIpcToken(token)) {
            ErrorLine{}
                << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
            return 2;
        }
        hasToken = true;
    }
    // IPC-007 真实供给：--ipc-token-file 优先（私有 ACL 存储），替代/覆盖内联明文。
    if (auto tokenFile = FindIpcTokenFile(argc, argv, 3)) {
        auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
        if (!stored) {
            ErrorLine{} << L"  --ipc-token-file unreadable; run "
                          L"--ipc-credential provision first\n";
            return 2;
        }
        token = stored.Value();
        hasToken = true;
    }
    const std::uint32_t frames = FindIpcFrames(argc, argv, 3);
    if (frames == 0 || frames > 16) {
        ErrorLine{} << L"  --frames must be 1..16\n";
        return 2;
    }
    const bool sessionMode = frames > 1;

    // 构建一帧结构化事实载荷（IPC-003 v1 schema：client_pid + 真实内存观测 +
    // observer；可附 agent_token 凭据事实）。每次调用重取内存（逐帧可能变化），
    // QueryMemoryStatus 只读无副作用。失败已打印原因，返回空。
    const auto buildFactsPayload =
        [&]() -> std::optional<
            std::pair<std::vector<optimizer::ipc::IpcFact>,
                      std::vector<std::byte>>> {
        auto memoryStatus = optimizer::memory::QueryMemoryStatus();
        if (!memoryStatus) {
            ErrorLine{} << L"  memory query failed ["
                       << optimizer::common::ToString(
                              memoryStatus.ErrorValue().domain)
                       << L"] " << memoryStatus.ErrorValue().message << L"\n";
            return std::nullopt;
        }
        const auto& memory = memoryStatus.Value();
        std::vector<optimizer::ipc::IpcFact> facts;
        facts.push_back(optimizer::ipc::IpcFact{
            "client_pid", std::to_string(::GetCurrentProcessId())});
        facts.push_back(optimizer::ipc::IpcFact{
            "memory_total_mb",
            std::to_string(memory.totalPhysicalBytes / (1024 * 1024))});
        facts.push_back(optimizer::ipc::IpcFact{
            "memory_available_mb",
            std::to_string(memory.availablePhysicalBytes / (1024 * 1024))});
        facts.push_back(optimizer::ipc::IpcFact{
            "memory_load_percent",
            std::to_string(memory.memoryLoadPercent)});
        facts.push_back(
            optimizer::ipc::IpcFact{"observer", "CppOptimizer ipc demo"});
        if (hasToken) {
            // 会话凭据事实（IPC-005）：ASCII 窄化（已在 IsValidIpcToken 约束）；
            // 不打印明文（FormatFactsSummary 亦不回显凭据）。
            std::string narrowToken;
            narrowToken.reserve(token.size());
            for (const wchar_t ch : token) {
                narrowToken.push_back(static_cast<char>(ch));
            }
            facts.push_back(optimizer::ipc::IpcFact{
                std::string(optimizer::ipc::kFactsTokenKey), narrowToken});
        }
        auto encoded = optimizer::ipc::SerializeFactsV1(facts);
        if (!encoded) {
            ErrorLine{} << L"  serialize facts failed ["
                       << optimizer::common::ToString(
                              encoded.ErrorValue().domain)
                       << L"] " << encoded.ErrorValue().message << L"\n";
            return std::nullopt;
        }
        return std::make_pair(std::move(facts), std::move(encoded).Value());
    };

    // 应答显示（默认处理器 Ack 载荷为 ASCII 文本，可直接显示）。
    const auto printReply = [](std::uint32_t requestId,
                               const optimizer::ipc::IpcReply& value) {
        std::wcout << L"  reply    : "
                   << optimizer::ipc::MessageTypeToString(value.type)
                   << L" id=" << requestId << L" payload="
                   << value.payload.size() << L" bytes";
        if (value.type == optimizer::ipc::IpcMessageType::Ack &&
            !value.payload.empty()) {
            std::string text;
            text.reserve(value.payload.size());
            for (const auto b : value.payload) {
                text.push_back(
                    static_cast<char>(static_cast<unsigned char>(b)));
            }
            std::wcout << L" [" << text.c_str() << L"]";
        }
        std::wcout << L"\n";
    };

    std::wcout << L"IPC pipe client (protected transport)\n";
    std::wcout << L"  pipe     : " << pipeName << L"\n";
    if (hasToken) {
        std::wcout << L"  auth     : session token supplied (hidden)\n";
    }
    if (sessionMode) {
        std::wcout << L"  session  : " << frames
                   << L" frames on one connection (no reconnect)\n";
    }

    auto backend = optimizer::ipc::CreateWin32ClientBackend();
    const std::chrono::milliseconds kIpcTimeout(3000);

    if (!sessionMode) {
        auto built = buildFactsPayload();
        if (!built) {
            return 2;
        }
        const std::vector<std::byte>& payload = built->second;
        std::wcout << L"  request  : FactsSnapshot id=1 payload=" << payload.size()
                   << L" bytes (" << built->first.size() << L" facts)\n";
        std::wcout << L"  facts    : "
                   << optimizer::ipc::FormatFactsSummary(built->first).c_str()
                   << L"\n";
        auto reply = optimizer::ipc::IpcRoundTrip(
            backend, pipeName, optimizer::ipc::IpcMessageType::FactsSnapshot,
            payload, 1, kIpcTimeout);
        if (!reply) {
            const auto& error = reply.ErrorValue();
            ErrorLine{} << L"  round trip failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        printReply(1, reply.Value());
        return 0;
    }

    // 多帧会话：连接一次，逐帧发送（每帧独立真实内存观测与自报身份）。
    std::vector<optimizer::ipc::IpcFrameRequest> requests;
    requests.reserve(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        auto built = buildFactsPayload();
        if (!built) {
            return 2;
        }
        if (i == 0) {
            std::wcout << L"  request  : FactsSnapshot id=1.." << frames
                       << L" payload=" << built->second.size() << L" bytes ("
                       << built->first.size() << L" facts each)\n";
            std::wcout << L"  facts    : "
                       << optimizer::ipc::FormatFactsSummary(built->first).c_str()
                       << L"\n";
        }
        optimizer::ipc::IpcFrameRequest request;
        request.type = optimizer::ipc::IpcMessageType::FactsSnapshot;
        request.requestId = i + 1;
        request.payload = std::move(built->second);
        requests.push_back(std::move(request));
    }
    auto replies = optimizer::ipc::IpcRoundTripSession(backend, pipeName,
                                                       requests, kIpcTimeout);
    if (!replies) {
        const auto& error = replies.ErrorValue();
        ErrorLine{} << L"  round trip session failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    for (std::size_t i = 0; i < replies.Value().size(); ++i) {
        printReply(static_cast<std::uint32_t>(i + 1), replies.Value()[i]);
    }
    return 0;
}

void PrintUsage() {
    std::wcout
        << L"CppOptimizer (engineering baseline)\n\n"
        << L"Usage:\n"
        << L"  CppOptimizer.exe --diagnose   Show safe, read-only platform diagnostics\n"
        << L"  CppOptimizer.exe --status     Show one read-only memory snapshot\n"
        << L"  CppOptimizer.exe --observe <s> [threshold] Sample each second for 1..60 s;\n"
        << L"                             optional low-load threshold 0..100 (default 50)\n"
        << L"  CppOptimizer.exe --activity <s> [idle-secs] [--session] [--foreground]  Observe\n"
        << L"                             user input activity (read-only "
           L"GetLastInputInfo; no\n"
        << L"                             hooks) for 1..60 s; idle threshold 1..3600 s (default\n"
        << L"                             15); outputs per-second Active/Idle/Unknown\n"
        << L"                             states. --session (ACT-002) adds read-only session\n"
        << L"                             context: remote/local, locked (WTS SessionFlags)\n"
        << L"                             and disconnected link states. --foreground (ACT-003)\n"
        << L"                             adds the foreground window owning pid\n"
        << L"                             (GetForegroundWindow/GetWindowThreadProcessId) on\n"
        << L"                             active/idle samples only\n"

        << L"  CppOptimizer.exe --agent-form <status|install|remove|apply> [form] [config] [--replace|--dry-run]\n"
        << L"                             Report, apply one form, or converge to the declared form\n"
        << L"                             (startup_tray|task|service); only one form may be active;\n"
        << L"                             a declared form never installs itself; registration needs admin\n"
        << L"  CppOptimizer.exe --service status  Report the SCM service install state (read-only)\n"
        << L"  CppOptimizer.exe --startup <status|install|remove>  Per-user startup entry\n"
        << L"                             (HKCU Run; R1, reversible, standard user;\n"
        << L"                             never installed by default)\n"
        << L"  CppOptimizer.exe --log <module> <message...> Write one Info log line to stderr\n"
        << L"                             (read-only, foreground, bounded)\n"
"\n"
        << L"  CppOptimizer.exe --scheduled-task <status|install|remove> [name]  Maintain the\n"
        << L"                             per-user logon-triggered scheduled task (registration needs\n"
        << L"                             admin; the task itself never runs elevated)\n"
        << L"  CppOptimizer.exe --journal [path] [lines]  Read back the action journal\n"
        << L"                             (read-only; before/after/state entries; local only)\n"
        << L"  CppOptimizer.exe --audit-log [path] [lines]  Read back the tail of the\n"
        << L"                             persisted audit trail (read-only; default: per-user\n"
        << L"                             audit.log, last 20 lines, max 200)\n"
        << L"  CppOptimizer.exe --audit-compact [path]  Compact the audit trail (two stages: the\n"
        << L"                             audit file compacts past 5 MiB; the summary file folds\n"
        << L"                             past 20 MiB; counts are never deleted)\n"
        << L"  CppOptimizer.exe --audit-summary [path] [lines]  Read back compacted summary\n"
        << L"                             totals (read-only; default: per-user summary file,\n"
        << L"                             last 20 lines, max 200)\n"
        << L"  CppOptimizer.exe --memory-clean [config.toml] [--dry-run]  Plan a memory clean and\n"
        << L"                             show the gates (no system call is made)\n"
        << L"  CppOptimizer.exe --memory-clean <config.toml> --execute-self-trim\n"
        << L"                             [--acknowledge-system-wide-side-effects]  Trim the working\n"
        << L"                             set of this process (R1, reversible; all six safety gates\n"
        << L"                             must pass; audit must be writable; one call, own process)\n"
        << L"  CppOptimizer.exe --gates [config.toml]  Report the six safety gates per dangerous\n"
        << L"                             capability (read-only; performs nothing)\n"
        << L"  CppOptimizer.exe --config <path>  Parse and validate a TOML config file\n"
        << L"  CppOptimizer.exe --cpu          Sample CPU usage (read-only, PDH)\n"
        << L"  CppOptimizer.exe --watch <s> [config.toml]  Watch game process lifecycle\n"
        << L"                             for 1..60 s (read-only, foreground, Toolhelp)\n"
        << L"  CppOptimizer.exe --list-processes [--all] [filter]  List running processes\n"
        << L"                             (read-only; default: visible windows only)\n"
        << L"  CppOptimizer.exe --add-game [pid] [main.toml] [--dry-run]  Add a running\n"
        << L"                             process as a game rule into config.local.toml\n"
        << L"                             (interactive picker when pid is omitted)\n"
        << L"  CppOptimizer.exe --policy <s> [config.toml]  Evaluate policy\n"
        << L"                             decisions for 1..60 s (advisory; R1\n"
        << L"                             executors act only when config gates\n"
        << L"                             [priority].enabled / [power].execution_required)\n"
        << L"  CppOptimizer.exe --power-lock <s> [execution|display|both]\n"
        << L"                             [reason...]  Hold a power request for 1..60 s\n"
        << L"                             (R1, reversible; released on exit)\n"
        << L"  CppOptimizer.exe --priority-boost <s> <pid> [config.toml]\n"
        << L"                             Temporarily raise a process priority class\n"
        << L"                             for 1..60 s (R1, reversible; level from\n"
        << L"                             [priority].max_level)\n"
        << L"  CppOptimizer.exe --service install [exe-path]  Register the Windows\n"
        << L"                             service (SCM, requires administrator;\n"
        << L"                             demand start, R0 workload)\n"
        << L"  CppOptimizer.exe --service uninstall           Remove the Windows service\n"
        << L"                             (SCM, requires administrator)\n"
        << L"  CppOptimizer.exe --service console <s|run> [config.toml] [--ipc-facts]\n"
        << L"                             [--confirm-recovery] [--ipc-token <t>]\n"
        << L"                             [--ipc-token-file [<path>]]\n"
        << L"                             [--ipc-allow-user <SID>]  Host the R0 workload\n"
        << L"                             in console mode for 1..60 s or until Ctrl+C\n"
        << L"                             (run = resident; foreground; Ctrl+C to stop).\n"
        << L"                             --ipc-facts also serves Agent facts frames via the\n"
        << L"                             protected pipe continuously\n"
        << L"                             (optional session token / user SID allow-list).\n"
        << L"                             --tray adds a notification-area icon whose\n"
        << L"                             right-click Exit menu stops the host.\n"
        << L"                             --confirm-recovery (IPC-018) acknowledges an\n"
        << L"                             unclean previous run (recovery-state.json marker)\n"
        << L"                             and clears the intake Safe Mode latch.\n"
        << L"                             Optional [config.toml] (first non-option arg)\n"
        << L"                             sets the Safe Mode intake gate from [ipc]\n"
        << L"                             (failures / window / cooldown / enabled;\n"
        << L"                             defaults 3 / 5 s / 2 s; out-of-range rejected)\n"
        << L"  CppOptimizer.exe CppOptimizerService  Service entry (started by SCM;\n"
        << L"                             equivalent to --service service)\n"
        << L"  CppOptimizer.exe --agent run <s> [suffix] [--interval-ms <50..10000>]\n"
        << L"                             [--max-interval-ms <n>] [--away-after-seconds <n>]\n"
        << L"                             [--away-cap-ms <n>] [--ipc-token <t>]\n"
        << L"                             [--ipc-token-file [<path>]]  Agent reporter\n"
        << L"                             (foreground, bounded): every interval ms collect\n"
        << L"                             real memory facts and report over the protected\n"
        << L"                             pipe for 1..60 s; bounded connect retries/backoff\n"
        << L"                             per report; repeated failures grow the gap up to\n"
        << L"                             max-interval-ms (default max(3x, 5 s)); optional\n"
        << L"                             session token; window-summary output.\n"
        << L"                             --away-after-seconds (ACT-007): when user idle\n"
        << L"                             reaches N s grow the gap up to --away-cap-ms\n"
        << L"                             (default max(3x,5s)); resume on input\n"
        << L"  CppOptimizer.exe --ipc-pipe server <s> [suffix] [--session]\n"
        << L"                             [--instances <1..8>] [--ipc-token <t>]\n"
        << L"                             Protected pipe server for 1..60 s: serve one\n"
        << L"                             client frame (strict validation; facts parsed\n"
        << L"                             + v1 schema whitelist; optional session token\n"
        << L"                             / user SID allow-list; ack/error reply).\n"
        << L"                             --session: serve multiple frames on one\n"
        << L"                             connection until client close / idle.\n"
        << L"                             --instances n (>1): n pipe instances /\n"
        << L"                             workers accept and serve clients concurrently\n"
        << L"                             (each connection multi-frame; bounded window)\n"
        << L"  CppOptimizer.exe --ipc-pipe client [suffix] [--frames <1..16>]\n"
        << L"                             [--ipc-token <t>]  Send real memory facts\n"
        << L"                             snapshots (CPOPFACTS/1; optional token) and\n"
        << L"                             print each reply. --frames n: reuse one\n"
        << L"                             connection for n frames (no reconnect;\n"
        << L"                             default 1)\n"
        << L"  CppOptimizer.exe --ipc-credential provision [path] [--force]\n"
        << L"                             Create/rotate the per-user private agent\n"
        << L"                             token store (IPC-007; ACL: SYSTEM + user;\n"
        << L"                             token never printed)\n"
        << L"  CppOptimizer.exe --ipc-credential status [path]  Show whether the\n"
        << L"                             token store is provisioned (hidden)\n"
        << L"  (server/client also accept --ipc-token-file [<path>] to use the store)\n"

        << L"  CppOptimizer.exe --help       Show this message\n\n"
        << L"No optimization action is enabled in this build entry point.\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--diagnose") {
            return RunDiagnostics();
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--status") {
            return RunStatus(L"");
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--status") {
            return RunStatus(argv[2]);
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--cpu") {
            return RunCpuCommand();
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--observe") {
            return RunObserve(argv[2], L"");
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--observe") {
            // 第 3 个参数：能当阈值就是阈值，否则按配置路径处理（CFG-006）。
            std::uint32_t threshold = 0;
            if (ParseUint32(argv[3], threshold)) {
                return RunObserve(argv[2], argv[3]);
            }
            return RunObserve(argv[2], L"", argv[3]);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--log") {
            return RunLogCommand(argc, argv);
        }
        if (argc >= 2 && argc <= 4 &&
            std::wstring_view(argv[1]) == L"--journal") {
            return RunJournalCommand(argc, argv);
        }
        if (argc >= 2 && argc <= 4 &&
            std::wstring_view(argv[1]) == L"--audit-log") {
            return RunAuditLogCommand(argc, argv);
        }
        if ((argc == 2 || argc == 3) &&
            std::wstring_view(argv[1]) == L"--audit-compact") {
            return RunAuditCompactCommand(argc, argv);
        }
        if (argc >= 2 && argc <= 4 &&
            std::wstring_view(argv[1]) == L"--audit-summary") {
            return RunAuditSummaryCommand(argc, argv);
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--memory-clean") {
            return RunMemoryCleanCommand(argc, argv);
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--gates") {
            return RunGatesCommand(argc, argv);
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--config") {
            return RunConfigCommand(argv[2]);
        }
        if ((argc >= 3 && argc <= 6) &&
            std::wstring_view(argv[1]) == L"--activity") {
            return RunActivityCommand(argc, argv);
        }
        if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--watch") {
            return RunWatchCommand(argc, argv);
        }
        if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--policy") {
            return RunPolicyCommand(argc, argv);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--power-lock") {
            return RunPowerLockCommand(argc, argv);
        }
        if ((argc == 4 || argc == 5) &&
            std::wstring_view(argv[1]) == L"--priority-boost") {
            return RunPriorityBoostCommand(argc, argv);
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--list-processes") {
            return RunListProcesses(argc, argv);
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--add-game") {
            return RunAddGameCommand(argc, argv);
        }
        if (argc == 2 && std::wstring_view(argv[1]) == kServiceName) {
            // SCM 启动入口：SCM 以服务名作为首个参数启动本进程。
            return RunServiceAsServiceCommand();
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--service") {
            const auto mode = optimizer::service::ParseRunMode(argv[2]);
            if (!mode) {
                ErrorLine{}
                    << L"  --service requires console|service|install|uninstall\n";
                return 2;
            }
            switch (*mode) {
                case optimizer::service::RunMode::Console:
                    return RunServiceConsoleCommand(argc, argv);
                case optimizer::service::RunMode::Service:
                    return RunServiceAsServiceCommand();
                case optimizer::service::RunMode::Install:
                    return RunServiceInstallCommand(argc, argv);
                case optimizer::service::RunMode::Uninstall:
                    return RunServiceUninstallCommand();
                case optimizer::service::RunMode::Status:
                    return RunServiceStatusCommand();
            }
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--agent") {
            if (std::wstring_view(argv[2]) != L"run") {
                ErrorLine{} << L"  --agent requires run\n";
                return 2;
            }
            return RunAgentCommand(argc, argv);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--ipc-pipe") {
            if (std::wstring_view(argv[2]) == L"server") {
                return RunIpcServerCommand(argc, argv);
            }
            if (std::wstring_view(argv[2]) == L"client") {
                return RunIpcClientCommand(argc, argv);
            }
            ErrorLine{} << L"  --ipc-pipe requires server|client\n";
            return 2;
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--startup") {
            return RunStartupCommand(argc, argv);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--scheduled-task") {
            return RunScheduledTaskCommand(argc, argv);
        }
        if (argc >= 2 && argc <= 6 &&
            std::wstring_view(argv[1]) == L"--agent-form") {
            return RunAgentFormCommand(argc, argv);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--ipc-credential") {
            return RunIpcCredentialCommand(argc, argv);
        }
        PrintUsage();
        return argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help") ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal C++ exception: " << exception.what() << '\n';
        return 100;
    } catch (...) {
        std::cerr << "Fatal unknown exception\n";
        return 101;
    }
}
