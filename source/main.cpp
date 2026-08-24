#include "common/console_output.hpp"
#include "common/error.hpp"
#include "config/config_manager.hpp"
#include "logger/logger.hpp"
#include "memory/memory_tuner.hpp"
#include "metrics/memory_metrics.hpp"
#include "metrics/pdh_metrics.hpp"
#include "platform/native_api.hpp"
#include "policy/policy_engine.hpp"
#include "power/power_locker.hpp"
#include "priority/priority_booster.hpp"
#include "process/process_watcher.hpp"

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
               << c.policy.cooldownMs << L" ms\n";
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
    // --policy <s> [config.toml]：前台、有界、只读的策略决策观测窗口。
    // 每秒：内存余量（只读查询）-> 压力分级 -> 游戏焦点（单轮 Toolhelp 轮询）
    // -> 规则评估 + 防抖，输出只读咨询决策。
    // v1 无任何执行器：所有决策均为建议，不产生系统修改；
    // 无效/缺失指标不触发决策（黄色不变量）。
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
        games = config.Value().games;
        policyConfig = config.Value().policy;
    } else {
        std::error_code existsError;
        if (std::filesystem::exists(
                std::filesystem::path(L"config.local.toml"), existsError)) {
            auto local = optimizer::config::LoadConfig(L"config.local.toml");
            if (local) {
                games = std::move(local.Value().games);
                policyConfig = local.Value().policy;
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

    std::wcout << L"Policy decision (read-only advisory, no system changes)\n";
    std::wcout << L"  rules : " << games.size() << L" game rule(s)\n";
    if (games.empty()) {
        std::wcout
            << L"  (pass --policy <s> <config.toml> to evaluate game rules)\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::size_t tick = 1;
    std::map<optimizer::policy::PolicyAction, int> counts;

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

        // 游戏焦点（单轮前台轮询，只读，不启动后台线程）。
        optimizer::policy::GameFocus focus;
        if (auto polled = watcher.PollOnce(); polled) {
            for (const auto& info : watcher.GetTrackedProcesses()) {
                if (info.state == optimizer::process::ProcessState::Running ||
                    info.state == optimizer::process::ProcessState::Starting) {
                    focus.gameId = info.gameId;
                    focus.running = true;
                    focus.foreground = info.isForeground;
                    const auto it = pauseByGame.find(info.gameId);
                    focus.pauseWhenBackground =
                        it == pauseByGame.end() ? true : it->second;
                    break; // v1：取配置顺序首个运行游戏
                }
            }
        }

        std::wostringstream line;
        if (!margin) {
            // 无效/缺失指标：不触发任何决策（黄色不变量）。
            line << L"  [" << tick
                 << L"s] margin n/a (memory query unavailable; no decision)";
            optimizer::common::WriteConsoleLine(line.str());
            if (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            ++tick;
            continue;
        }

        optimizer::policy::PolicyInput input;
        auto pressure = optimizer::policy::ClassifyPressure(
            static_cast<std::int32_t>(*margin), thresholds);
        if (!pressure) {
            // 阈值非法（配置层已拦截，此处防御）：不决策。
            line << L"  [" << tick
                 << L"s] policy thresholds invalid; no decision";
            optimizer::common::WriteConsoleLine(line.str());
            if (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            ++tick;
            continue;
        }
        input.pressure = pressure.Value();
        input.game = focus;

        const auto evaluation = evaluator.Evaluate(input, now);
        ++counts[evaluation.decision.action];

        line << L"  [" << tick << L"s] margin " << *margin << L"% "
             << optimizer::policy::PressureToString(input.pressure) << L" game=";
        if (focus.running) {
            line << std::wstring(focus.gameId.begin(), focus.gameId.end())
                 << L" fg=" << (focus.foreground ? L"yes" : L"no");
        } else {
            line << L"none";
        }
        line << L" -> "
             << optimizer::policy::ActionToString(evaluation.decision.action)
             << L" ("
             << std::wstring(evaluation.decision.reasonCode.begin(),
                             evaluation.decision.reasonCode.end());
        if (evaluation.suppressed) {
            line << L", cooldown";
        }
        line << L")";
        optimizer::common::WriteConsoleLine(line.str());

        if (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ++tick;
    }

    std::wostringstream summary;
    summary << L"  summary : NoOp "
            << counts[optimizer::policy::PolicyAction::NoOp] << L" / Notify "
            << counts[optimizer::policy::PolicyAction::Notify]
            << L" / SuggestMemoryTune "
            << counts[optimizer::policy::PolicyAction::SuggestMemoryTune]
            << L" / SuggestPriorityBoost "
            << counts[optimizer::policy::PolicyAction::SuggestPriorityBoost];
    optimizer::common::WriteConsoleLine(summary.str());
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
        << L"  CppOptimizer.exe --add-game [pid] [main.toml] [--dry-run]  Add a running\n"
        << L"                             process as a game rule into config.local.toml\n"
        << L"                             (interactive picker when pid is omitted)\n"
        << L"  CppOptimizer.exe --policy <s> [config.toml]  Evaluate advisory policy\n"
        << L"                             decisions for 1..60 s (read-only, no changes)\n"
        << L"  CppOptimizer.exe --power-lock <s> [execution|display|both]\n"
        << L"                             [reason...]  Hold a power request for 1..60 s\n"
        << L"                             (R1, reversible; released on exit)\n"
        << L"  CppOptimizer.exe --priority-boost <s> <pid> [config.toml]\n"
        << L"                             Temporarily raise a process priority class\n"
        << L"                             for 1..60 s (R1, reversible; level from\n"
        << L"                             [priority].max_level)\n"
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
