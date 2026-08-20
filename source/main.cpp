#include "common/console_output.hpp"
#include "common/error.hpp"
#include "config/config_manager.hpp"
#include "logger/logger.hpp"
#include "memory/memory_tuner.hpp"
#include "metrics/memory_metrics.hpp"
#include "metrics/pdh_metrics.hpp"
#include "platform/native_api.hpp"
#include "process/process_watcher.hpp"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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
    std::wcout << L"  games      : " << c.games.size() << L" rule(s)\n";
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
        auto config = optimizer::config::LoadConfig(argv[3]);
        if (!config) {
            const auto& error = config.ErrorValue();
            std::wcerr << L"  config load failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        games = std::move(config.Value().games);
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

void PrintUsage() {
    std::wcout
        << L"CppOptimizer (engineering baseline)\n\n"
        << L"Usage:\n"
        << L"  CppOptimizer.exe --diagnose   Show safe, read-only platform diagnostics\n"
        << L"  CppOptimizer.exe --status     Show one read-only memory snapshot\n"
        << L"  CppOptimizer.exe --observe <s> [threshold] Sample each second for 1..60 s;\n"
        << L"                             optional low-load threshold 0..100 (default 50)\n"
        << L"  CppOptimizer.exe --log <module> <message...> Write one Info log line to stderr\n"
        << L"                             (read-only, foreground, bounded)\n"
        << L"  CppOptimizer.exe --config <path>  Parse and validate a TOML config file\n"
        << L"  CppOptimizer.exe --cpu          Sample CPU usage (read-only, PDH)\n"
        << L"  CppOptimizer.exe --watch <s> [config.toml]  Watch game process lifecycle\n"
        << L"                             for 1..60 s (read-only, foreground, Toolhelp)\n"
        << L"  CppOptimizer.exe --list-processes [--all] [filter]  List running processes\n"
        << L"                             (read-only; default: visible windows only)\n"
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
        if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--watch") {
            return RunWatchCommand(argc, argv);
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--list-processes") {
            return RunListProcesses(argc, argv);
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
