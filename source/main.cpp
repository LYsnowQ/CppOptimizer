#include "common/console_output.hpp"
#include "common/error.hpp"
#include "activity/user_activity.hpp"
#include "config/config_manager.hpp"
#include "ipc/ipc_facts.hpp"
#include "ipc/ipc_credentials.hpp"
#include "ipc/ipc_protocol.hpp"
#include "ipc/ipc_session.hpp"
#include "ipc/ipc_transport.hpp"
#include "logger/logger.hpp"
#include "memory/memory_tuner.hpp"
#include "metrics/memory_metrics.hpp"
#include "metrics/pdh_metrics.hpp"
#include "platform/native_api.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_executor.hpp"
#include "power/power_locker.hpp"
#include "priority/priority_booster.hpp"
#include "process/process_watcher.hpp"
#include "service/service_host.hpp"

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

int RunStatus() {
    const auto result = optimizer::memory::QueryMemoryStatus();
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wcerr << L"  memory snapshot : failed ["
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

int RunObserve(std::wstring_view secondsText, std::wstring_view thresholdText) {
    // 前台、有界、用户主动发起的观测：无后台线程、无周期任务、无系统写入。
    // 属指标采样而非清理轮询；自动内存清理的红区规则不适用。
    constexpr std::uint32_t kMaxSeconds = 60;
    constexpr std::uint32_t kDefaultLowLoadThreshold = 50;
    std::uint32_t seconds = 0;
    if (!ParseUint32(secondsText, seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        std::wcerr << L"  --observe seconds must be in 1.." << kMaxSeconds << L"\n";
        return 2;
    }

    // 可选低负载阈值 0..100，默认 50。参数解析仅在命令层；纯函数将再次校验范围。
    std::uint32_t threshold = kDefaultLowLoadThreshold;
    if (!thresholdText.empty()) {
        std::uint32_t parsed = 0;
        if (!ParseUint32(thresholdText, parsed) || parsed > 100) {
            std::wcerr << L"  --observe threshold must be in 0..100\n";
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
            std::wcerr << L"  memory sample failed ["
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
        std::wcerr << L"  window aggregation failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    auto share = optimizer::metrics::ShareOfLoadBelow(samples, threshold);
    if (!share) {
        const auto& error = share.ErrorValue();
        std::wcerr << L"  low-load share failed ["
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

int RunConfigCommand(std::wstring_view path) {
    // --config <path>: 解析并校验 TOML 配置，输出关键项，只读。
    auto result = optimizer::config::LoadConfig(path);
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wcerr << L"  config load failed ["
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
    std::wcout << L"  version    : " << c.version << L"\n";
    std::wcout << L"  mode       : " << mode << L"\n";
    std::wcout << L"  logging    : level " << std::wstring(c.logging.level.begin(),
                                                        c.logging.level.end())
               << L", max " << c.logging.maxFileMb << L" MB x "
               << c.logging.maxFiles << L" files\n";
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
    std::wcout << L"\n";
    std::wcout << L"  games      : " << c.games.size() << L" rule(s)\n";
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
        std::wcerr << L"  --activity seconds must be in 1.." << kMaxSeconds
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
            std::wcerr << L"  --activity unknown option: " << arg << L"\n";
            return 2;
        }
        if (idleGiven || !ParseUint32(arg, idleSecs) || idleSecs == 0 ||
            idleSecs > 3600) {
            std::wcerr << L"  --activity idle-secs must be in 1..3600\n";
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
            std::wcerr << L"  observation failed ["
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
            std::wcerr << L"  observation failed ["
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
        std::wcerr << L"  observation failed ["
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
        std::wcerr << L"  cpu query init failed ["
                   << optimizer::common::ToString(init.ErrorValue().domain) << L":"
                   << init.ErrorValue().code << L"] "
                   << init.ErrorValue().message << L"\n";
        return 2;
    }

    auto first = query.Sample(); // warming-up 基线
    if (!first) {
        std::wcerr << L"  cpu sample failed ["
                   << optimizer::common::ToString(first.ErrorValue().domain)
                   << L":" << first.ErrorValue().code << L"] "
                   << first.ErrorValue().message << L"\n";
        return 2;
    }

    auto second = query.Sample();
    if (!second || !second.Value().valid) {
        std::wcerr << L"  cpu sample not ready (warming up)\n";
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
        std::wcerr << L"  --log requires a module and a message\n";
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

int RunWatchCommand(int argc, wchar_t* argv[]) {
    // --watch <seconds> [config-path]: 前台、有界、只读的进程生命周期观测。
    // 后台轮询线程仅在命令执行期间存在，命令结束后立即停止。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (!ParseUint32(argv[2], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        std::wcerr << L"  --watch seconds must be in 1.." << kMaxSeconds << L"\n";
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
            std::wcerr << L"  config load failed ["
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
        std::wcerr << L"  rule setup failed ["
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
        std::wcerr << L"  watcher start failed ["
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
        std::wcerr << L"  --policy seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }

    // 配置加载：main + 同目录 config.local.toml 合并（与 --watch 一致）；
    // 无 main 时加载当前目录 config.local.toml（存在时）。
    std::vector<optimizer::config::GameConfig> games;
    optimizer::config::PolicyConfig policyConfig;
    optimizer::config::PowerConfig powerConfig;
    optimizer::config::PriorityConfig priorityConfig;
    bool configLoaded = false; // 无配置时执行门禁全关（保守默认）
    if (argc >= 4) {
        const std::filesystem::path mainPath(argv[3]);
        const std::wstring localPath =
            (mainPath.parent_path() / L"config.local.toml").wstring();
        auto config =
            optimizer::config::LoadConfigWithLocal(argv[3], localPath);
        if (!config) {
            const auto& error = config.ErrorValue();
            std::wcerr << L"  config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        configLoaded = true;
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
        std::wcerr << L"  rule setup failed ["
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
    executorConfig.priorityEnabled =
        configLoaded && priorityConfig.enabled;
    executorConfig.priorityMaxLevel = priorityConfig.maxLevel;
    executorConfig.powerExecutionRequired =
        configLoaded && powerConfig.executionRequired;
    optimizer::priority::PriorityBooster::Options boosterOptions;
    boosterOptions.maxLevel = executorConfig.priorityMaxLevel;
    auto powerLocker = std::make_shared<optimizer::power::PowerLocker>(
        optimizer::power::CreateWin32Backend());
    auto booster = std::make_shared<optimizer::priority::PriorityBooster>(
        optimizer::priority::CreateWin32Backend(), boosterOptions);
    optimizer::policy::PolicyExecutor executor(powerLocker, booster,
                                               executorConfig);
    const bool executionOn =
        executorConfig.priorityEnabled || executorConfig.powerExecutionRequired;

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
                   << L"\n";
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
        optimizer::common::WriteConsoleLine(execSummary.str());
    }
    return 0;
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
        std::wcerr << L"  --power-lock seconds must be in 1.." << kMaxSeconds
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
        const auto acquired = locker.AcquireLock(type, reason);
        if (!acquired) {
            const auto& error = acquired.ErrorValue();
            std::wcerr
                << L"  acquire "
                << optimizer::power::PowerLockTypeToString(type)
                << L" failed [" << optimizer::common::ToString(error.domain)
                << L":" << error.code << L"] " << error.message << L"\n";
            locker.ReleaseAll();
            return 2;
        }
        std::wcout << L"  [acquired] "
                   << optimizer::power::PowerLockTypeToString(type) << L"\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::wcout << L"  holding for " << seconds << L" s ...\n";
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    locker.ReleaseAll();
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
        std::wcerr << L"  --priority-boost seconds must be in 1.."
                   << kMaxSeconds << L"\n";
        return 2;
    }
    std::uint32_t pid = 0;
    if (argc < 4 || !ParseUint32(argv[3], pid) || pid == 0) {
        std::wcerr << L"  --priority-boost requires a positive pid\n";
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
            std::wcerr << L"  config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        priorityConfig = config.Value().priority;
    }
    const auto level = priorityConfig.maxLevel;
    if (level == optimizer::config::PriorityLevel::None) {
        std::wcerr << L"  [priority].max_level is \"none\"; nothing to boost\n";
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

    const auto acquired =
        booster.AcquireBoost("cli-demo", pid, 0, level);
    if (!acquired) {
        const auto& error = acquired.ErrorValue();
        std::wcerr << L"  acquire failed ["
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
        std::wcerr << L"  process list failed ["
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
        std::wcerr << L"  native probe : failed ["
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
            std::wcerr << L"  unexpected argument: " << argv[i] << L"\n";
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
            std::wcerr << L"  process list failed ["
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
            std::wcerr << L"  query pid failed ["
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
        std::wcerr << L"  config load failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }
    auto rule = optimizer::process::BuildGameRuleFromProcess(
        selected.value(), existing.Value());
    if (!rule) {
        const auto& error = rule.ErrorValue();
        std::wcerr << L"  rule build failed ["
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
        std::wcerr << L"  write failed ["
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
    std::wstring ipcExpectedToken;                // 会话凭据（IPC-005；空 = 不要求）
    // Safe Mode（IPC-010/013，Agent 受理门禁）：时间窗口内身份/凭据失败达阈值暂停受理新 Agent。
    std::optional<optimizer::service::SafeModeGuard> ipcSafeMode; // console demo 启用
    bool ipcAuthRejected = false;                 // 最近一次 ServeOne 是否以身份/凭据拒绝结束
    std::size_t ipcSafeModeEntries = 0;           // 窗口内进入 Safe Mode 次数（汇总）
    std::size_t ipcRejectedClients = 0;           // 窗口内身份/凭据拒绝客户端数（汇总）
    // IPC-015：离散异常触发（Native capability 探测异常）——启动只读探测，异常锁存 Safe Mode。
    bool ipcNativeProbeOk = false;                // 启动探测通过（R0 baseline）
    bool ipcNativeProbeAnomaly = false;           // 启动探测异常（已触发锁存）
    std::size_t ipcAnomalyEntries = 0;            // 窗口内离散异常触发次数（汇总）
};

optimizer::common::Result<void> ServiceWorkloadTick(
    ServiceHostDemoState& state) noexcept {
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

    // SVC-002/003：受保护管道事实消费（--ipc-facts；常驻监听连续受理多客户端）。
    if (state.ipcEnabled && state.ipcSession) {
        state.ipcAuthRejected = false; // 每轮受理前复位（verdictObserver 按结论置位）
        // Safe Mode（IPC-010）：冷却期暂停受理新 Agent，R0 观测/日志照常。
        if (state.ipcSafeMode && !state.ipcSafeMode->ShouldAcceptClients()) {
            if (state.ipcSafeMode->IsAnomalyLatched()) {
                state.logger.Write(
                    optimizer::logger::LogLevel::Info, L"service",
                    L"ipc  : Safe Mode（Native 探测异常锁存）保持，暂停受理 Agent");
            } else {
                state.logger.Write(optimizer::logger::LogLevel::Info,
                                   L"service",
                                   L"ipc  : Safe Mode 冷却中，暂停受理 Agent");
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
                    } else {
                        state.ipcLastFactSummary.clear();
                        state.ipcLastFactCount = 0;
                        state.ipcLastUserIdleSeconds.reset();
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
        }
        state.logger.Write(optimizer::logger::LogLevel::Info, L"service",
                           servedMessage);
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
    if (const auto file = logger.SetFileSink(logPath.wstring()); !file) {
        logger.SetDebugSink();
    }
}

// IPC 凭据/选项解析辅助（前向声明，定义见 IPC 命令区段；服务控制台命令复用同套解析）。
bool FindIpcToken(int argc, wchar_t* argv[], int start,
                  std::wstring& out) noexcept;
bool IsValidIpcToken(const std::wstring& token) noexcept;
std::optional<std::filesystem::path> FindIpcTokenFile(
    int argc, wchar_t* argv[], int start) noexcept;

int RunServiceConsoleCommand(int argc, wchar_t* argv[]) {
    // --service console <s> [config.toml] [--ipc-facts]：控制台托管演示（前台、有界、
    // Ctrl+C 优雅停止）。可选 --ipc-facts 在负载窗口内同时作为受保护管道服务端常驻
    // 受理 Agent 事实（SVC-002/003，R0）。可选 [config.toml]（紧跟在秒数后的首个
    // 非选项参数）在 --ipc-facts 时经 [ipc] 节配置 Safe Mode 门禁窗口参数
    // （IPC-014：阈值/窗口/冷却/开关；缺省与 SafeModeGuard 默认一致）。
    constexpr std::uint32_t kMaxSeconds = 60;
    std::uint32_t seconds = 0;
    if (argc < 4 || !ParseUint32(argv[3], seconds) || seconds == 0 ||
        seconds > kMaxSeconds) {
        std::wcerr << L"  --service console seconds must be in 1.."
                   << kMaxSeconds << L"\n";
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
    for (int i = flagsStart; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--ipc-facts") {
            ipcFacts = true;
            break;
        }
    }

    // IPC-014：Safe Mode 门禁窗口参数（供 banner/门禁构造引用，缺省 = 状态机默认）。
    optimizer::service::SafeModeGuard::Options safeModeOptions;
    // [ipc] 配置仅在 --ipc-facts 时消费（无门禁则配置无意义）；无配置路径时保持
    // SafeModeGuard 默认常量（零回归）。
    std::optional<optimizer::config::ConfigSnapshot> consoleConfigSnapshot;
    if (ipcFacts && consoleConfig) {
        auto loaded = optimizer::config::LoadConfig(consoleConfig->wstring());
        if (!loaded) {
            const auto& error = loaded.ErrorValue();
            std::wcerr << L"  --service console config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        const auto& safeMode = loaded.Value().ipc.safeMode;
        safeModeOptions.enabled = safeMode.enabled;
        safeModeOptions.failuresToEnter =
            static_cast<std::size_t>(safeMode.failuresToEnter);
        safeModeOptions.countingWindow = std::chrono::milliseconds(
            safeMode.countingWindowMs);
        safeModeOptions.cooldown =
            std::chrono::milliseconds(safeMode.cooldownMs);
        consoleConfigSnapshot = loaded.Value();
    }

    ServiceHostDemoState state;
    state.logger.SetStderrSink();
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
                std::wcerr
                    << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
                return 2;
            }
            ipcOptions.expectedToken = ipcToken;
        }
        if (auto tokenFile = FindIpcTokenFile(argc, argv, flagsStart)) {
            auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
            if (!stored) {
                std::wcerr << L"  --ipc-token-file unreadable; run "
                              L"--ipc-credential provision first\n";
                return 2;
            }
            ipcOptions.expectedToken = stored.Value();
        }
        for (int i = flagsStart; i + 1 < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--ipc-allow-user") {
                const std::wstring sid = argv[i + 1];
                if (sid.empty() || sid.size() > 192) {
                    std::wcerr
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
        // IPC-015：Native capability 探测异常（docs/23 §6 触发项）。受理 Agent 前做一次只读
        // R0 baseline 探测：探测失败或核心只读能力缺失（ntdll 加载失败 / NtQuerySystemInformation
        // 或状态码转换不可用）视为持续状态异常——离散异常锁存 Safe Mode，整个窗口暂停受理新
        // Agent（R0 负载/日志照常）；探测正常则 banner 标注 ok。
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
        if (nativeAnomaly) {
            state.ipcNativeProbeAnomaly = true;
            state.ipcSafeMode->OnAnomalyDetected();
            ++state.ipcAnomalyEntries;
            state.logger.Write(
                optimizer::logger::LogLevel::Info, L"service",
                L"ipc  : Safe Mode entered - Native capability 探测异常"
                L"（R0 baseline 缺失），暂停受理新 Agent");
        } else {
            state.ipcNativeProbeOk = true;
        }
    }
    optimizer::service::ServiceHost::Options options;
    options.identity.name = kServiceName;
    options.identity.displayName = kServiceDisplayName;
    options.identity.description = kServiceDescription;
    optimizer::service::ServiceHost host(
        [&state] { return ServiceWorkloadTick(state); }, std::move(options),
        optimizer::service::CreateWin32ScmBackend());

    std::wostringstream header;
    header << L"Service host (console mode, R0 workload, " << seconds
           << L" s)";
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
            if (consoleConfigSnapshot) {
                optimizer::common::WriteConsoleLine(
                    L"  config   : " + consoleConfig.value().wstring() +
                    L" ([ipc] safe_mode)");
            }
            optimizer::common::WriteConsoleLine(
                state.ipcNativeProbeAnomaly
                    ? L"  native   : probe anomaly -> Safe Mode (agent intake "
                      L"paused for window)"
                    : L"  native   : probe ok (R0 read-only baseline)");
        }
    }
    optimizer::common::WriteConsoleLine(L"  stop     : Ctrl+C or timeout");
    const auto result = host.RunConsole(std::chrono::seconds(seconds));
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
        return 2;
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
            }
        } else {
            ipcLine << L"  ipc      : no Agent connected within window";
        }
        optimizer::common::WriteConsoleLine(ipcLine.str());
        if (state.ipcSafeMode) {
            std::wostringstream safeLine;
            safeLine << L"  safe mode: entered " << state.ipcSafeModeEntries
                     << L" time(s), rejected clients "
                     << state.ipcRejectedClients;
            if (state.ipcAnomalyEntries > 0) {
                safeLine << L", native probe anomaly "
                         << state.ipcAnomalyEntries;
            }
            optimizer::common::WriteConsoleLine(safeLine.str());
        }
    }
    std::wostringstream stoppedLine;
    stoppedLine << L"  stopped  : "
                << (host.IsStopRequested() ? L"user (Ctrl+C)" : L"timeout");
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
    std::wostringstream done;
    done << L"Service uninstalled: " << kServiceName;
    optimizer::common::WriteConsoleLine(done.str());
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
                                  arg == L"--max-interval-ms";
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
                                     arg == L"--max-interval-ms";
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
        std::wcerr << L"  --agent run seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }
    const std::wstring suffix = FindIpcSuffix(argc, argv, 4);
    const std::wstring pipeName = IpcPipeName(suffix);
    if (pipeName.empty()) {
        std::wcerr << L"  --agent suffix must be ASCII letters/digits/-/_\n";
        return 2;
    }
    const std::uint32_t intervalMs = FindIpcIntervalMs(argc, argv, 4);
    if (intervalMs == 0 || intervalMs < 50 || intervalMs > 10000) {
        std::wcerr << L"  --interval-ms must be in 50..10000\n";
        return 2;
    }
    // 自适应节奏放大上限（IPC-012）：显式须 >= interval；缺省 max(3*interval, 5s)。
    const std::uint32_t intervalCapMs = FindIpcMaxIntervalMs(argc, argv, 4);
    if (intervalCapMs != 0 &&
        (intervalCapMs < intervalMs || intervalCapMs > 60000)) {
        std::wcerr << L"  --max-interval-ms must be in [" << intervalMs
                   << L"..60000]\n";
        return 2;
    }
    const std::uint32_t effectiveCapMs =
        intervalCapMs != 0
            ? intervalCapMs
            : (intervalMs * 3 > 5000u ? intervalMs * 3 : 5000u);
    std::wstring token;
    bool hasToken = false;
    if (FindIpcToken(argc, argv, 4, token)) {
        if (!IsValidIpcToken(token)) {
            std::wcerr
                << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
            return 2;
        }
        hasToken = true;
    }
    if (auto tokenFile = FindIpcTokenFile(argc, argv, 4)) {
        auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
        if (!stored) {
            std::wcerr << L"  --ipc-token-file unreadable; run "
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

    std::wcout << L"Agent (periodic reporter, foreground bounded, " << seconds
               << L" s)\n";
    std::wcout << L"  pipe     : " << pipeName << L"\n";
    std::wcout << L"  report   : every " << intervalMs
               << L" ms (grow to " << effectiveCapMs
               << L" ms after repeated failures), up to 3 connect attempts per report\n";
    std::wcout
        << L"  facts    : memory (total/available/load) + user_idle_seconds "
           L"(ACT-005, GetLastInputInfo read-only; omitted on query failure)\n";
    if (hasToken) {
        std::wcout << L"  auth     : session token supplied (hidden)\n";
    }

    auto backend = optimizer::ipc::CreateWin32ClientBackend();
    auto result = optimizer::ipc::RunPeriodicReporter(backend, reportOptions,
                                                      buildRequest);
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wcerr << L"  agent run failed ["
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
        std::wcerr
            << L"  cannot locate LOCALAPPDATA; provide an explicit <path>\n";
        return 2;
    }
    if (action == L"provision") {
        if (auto result = optimizer::ipc::ProvisionAgentTokenFile(path, force);
            !result) {
            const auto& error = result.ErrorValue();
            std::wcerr << L"  provision failed ["
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
    std::wcerr << L"  --ipc-credential requires provision|status\n";
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
        std::wcerr << L"  --ipc-pipe server seconds must be in 1.." << kMaxSeconds
                   << L"\n";
        return 2;
    }
    const std::wstring suffix = FindIpcSuffix(argc, argv, 4);
    const std::wstring pipeName = IpcPipeName(suffix);
    if (pipeName.empty()) {
        std::wcerr << L"  --ipc-pipe suffix must be ASCII letters/digits/-/_\n";
        return 2;
    }

    optimizer::ipc::IpcSession::Options sessionOptions;
    std::wstring token;
    if (FindIpcToken(argc, argv, 4, token)) {
        if (!IsValidIpcToken(token)) {
            std::wcerr
                << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
            return 2;
        }
        sessionOptions.expectedToken = token; // 明文仅限 demo；真实供给属后续切片
    }
    // IPC-007 真实供给：--ipc-token-file 优先（私有 ACL 存储），替代/覆盖内联明文。
    if (auto tokenFile = FindIpcTokenFile(argc, argv, 4)) {
        auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
        if (!stored) {
            std::wcerr << L"  --ipc-token-file unreadable; run "
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
                std::wcerr << L"  --ipc-allow-user needs a valid SID string\n";
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
        std::wcerr << L"  --instances must be 1..8\n";
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
            std::wcerr << L"  concurrent serve failed ["
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
        std::wcerr << L"  serve failed ["
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
        std::wcerr << L"  --ipc-pipe suffix must be ASCII letters/digits/-/_\n";
        return 2;
    }
    std::wstring token;
    bool hasToken = false;
    if (FindIpcToken(argc, argv, 3, token)) {
        if (!IsValidIpcToken(token)) {
            std::wcerr
                << L"  --ipc-token must be 1..64 ASCII letters/digits/_/-\n";
            return 2;
        }
        hasToken = true;
    }
    // IPC-007 真实供给：--ipc-token-file 优先（私有 ACL 存储），替代/覆盖内联明文。
    if (auto tokenFile = FindIpcTokenFile(argc, argv, 3)) {
        auto stored = optimizer::ipc::ReadAgentTokenFile(*tokenFile);
        if (!stored) {
            std::wcerr << L"  --ipc-token-file unreadable; run "
                          L"--ipc-credential provision first\n";
            return 2;
        }
        token = stored.Value();
        hasToken = true;
    }
    const std::uint32_t frames = FindIpcFrames(argc, argv, 3);
    if (frames == 0 || frames > 16) {
        std::wcerr << L"  --frames must be 1..16\n";
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
            std::wcerr << L"  memory query failed ["
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
            std::wcerr << L"  serialize facts failed ["
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
            std::wcerr << L"  round trip failed ["
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
        std::wcerr << L"  round trip session failed ["
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

        << L"  CppOptimizer.exe --log <module> <message...> Write one Info log line to stderr\n"
        << L"                             (read-only, foreground, bounded)\n"
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
        << L"  CppOptimizer.exe --service console <s> [config.toml] [--ipc-facts]\n"
        << L"                             [--ipc-token <t>] [--ipc-token-file [<path>]]\n"
        << L"                             [--ipc-allow-user <SID>]  Host the R0 workload\n"
        << L"                             in console mode for 1..60 s (foreground; Ctrl+C\n"
        << L"                             to stop). --ipc-facts also serves Agent facts\n"
        << L"                             frames via the protected pipe continuously\n"
        << L"                             (optional session token / user SID allow-list).\n"
        << L"                             Optional [config.toml] (first non-option arg)\n"
        << L"                             sets the Safe Mode intake gate from [ipc]\n"
        << L"                             (failures / window / cooldown / enabled;\n"
        << L"                             defaults 3 / 5 s / 2 s; out-of-range rejected)\n"
        << L"  CppOptimizer.exe CppOptimizerService  Service entry (started by SCM;\n"
        << L"                             equivalent to --service service)\n"
        << L"  CppOptimizer.exe --agent run <s> [suffix] [--interval-ms <50..10000>]\n"
        << L"                             [--max-interval-ms <n>] [--ipc-token <t>]\n"
        << L"                             [--ipc-token-file [<path>]]  Agent reporter\n"
        << L"                             (foreground, bounded): every interval ms collect\n"
        << L"                             real memory facts and report over the protected\n"
        << L"                             pipe for 1..60 s; bounded connect retries/backoff\n"
        << L"                             per report; repeated failures grow the gap up to\n"
        << L"                             max-interval-ms (default max(3x, 5 s)); optional\n"
        << L"                             session token; window-summary output\n"
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
            return RunStatus();
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--cpu") {
            return RunCpuCommand();
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--observe") {
            return RunObserve(argv[2], L"");
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--observe") {
            return RunObserve(argv[2], argv[3]);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--log") {
            return RunLogCommand(argc, argv);
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
                std::wcerr
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
            }
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--agent") {
            if (std::wstring_view(argv[2]) != L"run") {
                std::wcerr << L"  --agent requires run\n";
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
            std::wcerr << L"  --ipc-pipe requires server|client\n";
            return 2;
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
