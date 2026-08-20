#include "process/process_watcher.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include "common/unique_resource.hpp"

#include <string>
#include <utility>

namespace optimizer::process {

namespace {

// UTF-8 窄字符串转宽字符串。非法 UTF-8 报错（MB_ERR_INVALID_CHARS）。
common::Result<std::wstring> Utf8ToWide(std::string_view text) noexcept {
    if (text.empty()) {
        return common::Result<std::wstring>::Success(L"");
    }
    const int size = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) {
        return common::Result<std::wstring>::Failure(
            common::Error::FromWin32(::GetLastError(), "MultiByteToWideChar"));
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(), size);
    return common::Result<std::wstring>::Success(std::move(wide));
}

// ASCII 字母折叠（A-Z -> a-z），其余字符原样。无区域依赖。
wchar_t FoldAscii(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return ch;
}

// 用 (gameId, pid) 在跟踪表中查找条目。
const ProcessInfo* FindTracked(std::span<const ProcessInfo> tracked,
                               std::string_view gameId,
                               std::uint32_t pid) noexcept {
    for (const auto& info : tracked) {
        if (info.gameId == gameId && info.pid == pid) {
            return &info;
        }
    }
    return nullptr;
}

// presence 中是否存在同 (gameId, pid) 条目。
bool PresenceHas(std::span<const GamePresence> presence,
                 std::string_view gameId, std::uint32_t pid) noexcept {
    for (const auto& item : presence) {
        if (item.gameId == gameId && item.pid == pid) {
            return true;
        }
    }
    return false;
}

ProcessInfo ToProcessInfo(const GamePresence& presence, ProcessState state,
                          std::uint64_t generation) noexcept {
    ProcessInfo info;
    info.gameId = presence.gameId;
    info.pid = presence.pid;
    info.processName = presence.processName;
    info.windowTitle = presence.windowTitle;
    info.isForeground = presence.isForeground;
    info.isFullscreen = presence.isFullscreen;
    info.creationTime100ns = presence.creationTime100ns;
    info.state = state;
    info.generation = generation;
    return info;
}

// 窗口枚举回调上下文。
struct WindowEnumContext {
    std::uint32_t pid = 0;
    std::wstring title;
    bool foundTitle = false;
    bool foundVisible = false;
};

BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lparam) noexcept {
    auto* context = reinterpret_cast<WindowEnumContext*>(lparam);
    if (!::IsWindowVisible(hwnd)) {
        return TRUE;
    }
    DWORD windowPid = 0;
    ::GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != context->pid) {
        return TRUE;
    }
    context->foundVisible = true;
    if (!context->foundTitle) {
        wchar_t buffer[512]{};
        const int length = ::GetWindowTextW(hwnd, buffer, 512);
        if (length > 0) {
            context->title.assign(buffer, static_cast<std::size_t>(length));
            context->foundTitle = true;
        }
    }
    return TRUE;
}

// 窗口矩形是否占满所在监视器（全屏判定）。
bool IsWindowFullscreen(HWND hwnd) noexcept {
    RECT windowRect{};
    if (!::GetWindowRect(hwnd, &windowRect)) {
        return false;
    }
    HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!::GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }
    return windowRect.left == monitorInfo.rcMonitor.left &&
           windowRect.top == monitorInfo.rcMonitor.top &&
           windowRect.right == monitorInfo.rcMonitor.right &&
           windowRect.bottom == monitorInfo.rcMonitor.bottom;
}

} // namespace

const wchar_t* StateToString(ProcessState state) noexcept {
    switch (state) {
        case ProcessState::NotRunning:
            return L"NotRunning";
        case ProcessState::Starting:
            return L"Starting";
        case ProcessState::Running:
            return L"Running";
        case ProcessState::Suspended:
            return L"Suspended";
        case ProcessState::Exiting:
            return L"Exiting";
    }
    return L"Unknown";
}

common::Result<std::vector<GameRule>> BuildGameRules(
    std::span<const config::GameConfig> games) noexcept {
    std::vector<GameRule> rules;
    rules.reserve(games.size());
    for (const auto& game : games) {
        GameRule rule;
        rule.id = game.id;
        rule.processNames.reserve(game.processNames.size());
        for (const auto& name : game.processNames) {
            auto wide = Utf8ToWide(name);
            if (!wide) {
                return common::Result<std::vector<GameRule>>::Failure(
                    wide.ErrorValue());
            }
            rule.processNames.push_back(std::move(wide.Value()));
        }
        rules.push_back(std::move(rule));
    }
    return common::Result<std::vector<GameRule>>::Success(std::move(rules));
}

bool ProcessNameMatches(std::wstring_view ruleName,
                        std::wstring_view processName) noexcept {
    if (ruleName.empty() || ruleName.size() != processName.size()) {
        return false;
    }
    for (std::size_t i = 0; i < ruleName.size(); ++i) {
        if (FoldAscii(ruleName[i]) != FoldAscii(processName[i])) {
            return false;
        }
    }
    return true;
}

std::vector<RuleMatch> MatchRulesToEntries(
    std::span<const GameRule> rules,
    std::span<const ProcessEntry> entries) noexcept {
    std::vector<RuleMatch> matched;
    matched.reserve(rules.size());
    for (const auto& rule : rules) {
        for (const auto& entry : entries) {
            bool hit = false;
            for (const auto& ruleName : rule.processNames) {
                if (ProcessNameMatches(ruleName, entry.name)) {
                    hit = true;
                    break;
                }
            }
            if (hit) {
                matched.push_back(RuleMatch{rule.id, entry});
                break;
            }
        }
    }
    return matched;
}

DiffResult DiffProcessPresence(
    std::span<const ProcessInfo> previous,
    std::span<const GamePresence> presence,
    std::uint64_t& nextGeneration) noexcept {
    DiffResult result;
    result.tracked.reserve(previous.size() + presence.size());

    // 第一遍：本轮在场进程。
    for (const auto& item : presence) {
        if (item.pid == 0) {
            continue; // 防御：非法 pid 跳过
        }
        const ProcessInfo* prev = FindTracked(previous, item.gameId, item.pid);
        if (prev == nullptr) {
            ProcessInfo info =
                ToProcessInfo(item, ProcessState::Starting, nextGeneration);
            ++nextGeneration;
            result.tracked.push_back(info);
            result.transitions.push_back(ProcessTransition{
                item.gameId, ProcessState::NotRunning, ProcessState::Starting, info});
            continue;
        }

        // 同一 pid 但创建时间不同（且都已知）：视为重启，旧生命周期结束。
        const bool restart = prev->creationTime100ns != 0 &&
                             item.creationTime100ns != 0 &&
                             prev->creationTime100ns != item.creationTime100ns;
        if (restart) {
            result.transitions.push_back(ProcessTransition{
                item.gameId, prev->state, ProcessState::Exiting, *prev});
            ProcessInfo info =
                ToProcessInfo(item, ProcessState::Starting, nextGeneration);
            ++nextGeneration;
            result.tracked.push_back(info);
            result.transitions.push_back(ProcessTransition{
                item.gameId, ProcessState::NotRunning, ProcessState::Starting, info});
            continue;
        }

        // 同一生命周期在场：推进到 Running（Starting/Exiting/Suspended 确认或恢复）。
        ProcessInfo info = ToProcessInfo(item, ProcessState::Running, prev->generation);
        if (prev->state != ProcessState::Running) {
            result.transitions.push_back(ProcessTransition{
                item.gameId, prev->state, ProcessState::Running, info});
        }
        result.tracked.push_back(info);
    }

    // 第二遍：上一轮在场但本轮不在场的条目。
    for (const auto& prev : previous) {
        if (PresenceHas(presence, prev.gameId, prev.pid)) {
            continue; // 第一遍已处理（含重启场景）
        }
        ProcessInfo info = prev;
        switch (prev.state) {
            case ProcessState::Starting:
            case ProcessState::Running:
            case ProcessState::Suspended:
                // 保留一轮 Exiting，下轮仍缺席再发 NotRunning。
                info.state = ProcessState::Exiting;
                result.tracked.push_back(info);
                result.transitions.push_back(ProcessTransition{
                    prev.gameId, prev.state, ProcessState::Exiting, info});
                break;
            case ProcessState::Exiting:
                // 生命周期结束，不再保留。
                result.transitions.push_back(ProcessTransition{
                    prev.gameId, prev.state, ProcessState::NotRunning, info});
                break;
            case ProcessState::NotRunning:
            default:
                break; // 防御：跟踪表中不应出现 NotRunning 条目
        }
    }
    return result;
}

common::Result<std::vector<ProcessEntry>> EnumerateProcesses() noexcept {
    common::UniqueHandle snapshot(
        ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.IsValid()) {
        return common::Result<std::vector<ProcessEntry>>::Failure(
            common::Error::FromWin32(::GetLastError(),
                                     "CreateToolhelp32Snapshot"));
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.Get(), &entry)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_NO_MORE_FILES) {
            return common::Result<std::vector<ProcessEntry>>::Success({});
        }
        return common::Result<std::vector<ProcessEntry>>::Failure(
            common::Error::FromWin32(code, "Process32FirstW"));
    }

    std::vector<ProcessEntry> processes;
    do {
        processes.push_back(
            ProcessEntry{entry.th32ProcessID, std::wstring(entry.szExeFile)});
    } while (::Process32NextW(snapshot.Get(), &entry));
    return common::Result<std::vector<ProcessEntry>>::Success(std::move(processes));
}

std::uint64_t QueryProcessCreationTime(std::uint32_t pid) noexcept {
    // 最小权限：只请求查询受限信息，绝不请求 PROCESS_ALL_ACCESS。
    common::UniqueHandle process(::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.IsValid()) {
        return 0;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!::GetProcessTimes(process.Get(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
           static_cast<std::uint64_t>(creation.dwLowDateTime);
}

common::Result<WindowInfo> QueryWindowInfo(std::uint32_t pid) noexcept {
    WindowInfo info;
    WindowEnumContext context;
    context.pid = pid;
    // 枚举失败降级为空信息，不阻断观测。
    if (::EnumWindows(EnumWindowProc, reinterpret_cast<LPARAM>(&context))) {
        info.title = std::move(context.title);
        info.hasVisibleWindow = context.foundVisible;
    }

    const HWND foreground = ::GetForegroundWindow();
    if (foreground != nullptr) {
        DWORD foregroundPid = 0;
        ::GetWindowThreadProcessId(foreground, &foregroundPid);
        if (foregroundPid == pid) {
            info.isForeground = true;
            info.isFullscreen = IsWindowFullscreen(foreground);
        }
    }
    return common::Result<WindowInfo>::Success(info);
}

// ---------- 进程目录 ----------

// ASCII 大小写不敏感的包含判断（无区域依赖）。
bool ContainsAscii(std::wstring_view text, std::wstring_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool matched = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (FoldAscii(text[i + j]) != FoldAscii(needle[j])) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

bool ProcessMatchesFilter(const ProcessDetails& details,
                          std::wstring_view filter) noexcept {
    if (filter.empty()) {
        return true;
    }
    return ContainsAscii(details.name, filter) ||
           ContainsAscii(details.executablePath, filter) ||
           ContainsAscii(details.windowTitle, filter);
}

common::Result<ProcessDetails> QueryProcessDetails(
    std::uint32_t pid) noexcept {
    // 最小权限：只请求查询受限信息，绝不请求 PROCESS_ALL_ACCESS。
    common::UniqueHandle process(::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.IsValid()) {
        return common::Result<ProcessDetails>::Failure(
            common::Error::FromWin32(::GetLastError(), "OpenProcess"));
    }

    ProcessDetails details;
    details.pid = pid;

    // 完整路径（进程可能已改名，当前名只作参考）。
    wchar_t pathBuffer[MAX_PATH]{};
    DWORD pathSize = MAX_PATH;
    if (::QueryFullProcessImageNameW(process.Get(), 0, pathBuffer, &pathSize)) {
        details.executablePath.assign(pathBuffer, pathSize);
        const auto pos = details.executablePath.find_last_of(L'\\');
        details.name = pos == std::wstring::npos
                           ? details.executablePath
                           : details.executablePath.substr(pos + 1);
    }

    // 内存占用（工作集）。
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(process.Get(), &counters, sizeof(counters))) {
        details.workingSetBytes = counters.WorkingSetSize;
    }

    // 窗口信息（尽力而为，失败不阻断）。
    const auto window = QueryWindowInfo(pid);
    if (window) {
        details.windowTitle = window.Value().title;
        details.hasVisibleWindow = window.Value().hasVisibleWindow;
        details.isForeground = window.Value().isForeground;
        details.isFullscreen = window.Value().isFullscreen;
    }
    return common::Result<ProcessDetails>::Success(details);
}

common::Result<std::vector<ProcessDetails>> EnumerateProcessDetails(
    bool windowOnly) noexcept {
    auto processes = EnumerateProcesses();
    if (!processes) {
        return common::Result<std::vector<ProcessDetails>>::Failure(
            processes.ErrorValue());
    }

    std::vector<ProcessDetails> result;
    result.reserve(processes.Value().size());
    for (const auto& entry : processes.Value()) {
        ProcessDetails details;
        auto query = QueryProcessDetails(entry.pid);
        if (query) {
            details = std::move(query.Value());
        } else {
            // 单进程查询失败降级为最小条目，不阻断列表。
            details.pid = entry.pid;
            details.name = entry.name;
        }
        if (!windowOnly || details.hasVisibleWindow) {
            result.push_back(std::move(details));
        }
    }
    return common::Result<std::vector<ProcessDetails>>::Success(std::move(result));
}

// ---------- ProcessWatcher ----------

ProcessWatcher::ProcessWatcher(Options options) noexcept
    : options_(options) {}

ProcessWatcher::~ProcessWatcher() noexcept {
    Stop();
}

common::Result<void> ProcessWatcher::SetRules(
    std::span<const config::GameConfig> games) noexcept {
    auto rules = BuildGameRules(games);
    if (!rules) {
        return common::Result<void>::Failure(rules.ErrorValue());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_ = std::move(rules.Value());
        tracked_.clear();
        nextGeneration_ = 1;
    }
    return common::Result<void>::Success();
}

void ProcessWatcher::Subscribe(ProcessEventCallback callback) noexcept {
    if (!callback) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(callback));
}

common::Result<void> ProcessWatcher::Start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) {
        return common::Result<void>::Success();
    }
    stopRequested_ = false;
    running_ = true;
    tracked_.clear();
    try {
        worker_ = std::thread([this] { WorkerLoop(); });
    } catch (const std::system_error&) {
        running_ = false;
        return common::Result<void>::Failure(common::Error::Validation(
            "ProcessWatcher::Start", L"Failed to create the polling thread"));
    }
    return common::Result<void>::Success();
}

void ProcessWatcher::Stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable()) {
            running_ = false;
            return;
        }
        stopRequested_ = true;
        running_ = false;
        stopCv_.notify_all();
    }
    // 防御：回调在轮询线程内调用 Stop()（契约违规）时不得自 join，
    // 由外部线程（析构等）负责 join 已退出的线程。
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

bool ProcessWatcher::IsRunning() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::vector<ProcessInfo> ProcessWatcher::GetTrackedProcesses() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracked_;
}

common::Result<std::vector<ProcessTransition>> ProcessWatcher::PollOnce() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    auto processes = EnumerateProcesses();
    if (!processes) {
        return common::Result<std::vector<ProcessTransition>>::Failure(
            processes.ErrorValue());
    }

    const auto matches = MatchRulesToEntries(rules_, processes.Value());
    std::vector<GamePresence> presence;
    presence.reserve(matches.size());
    for (const auto& match : matches) {
        GamePresence item;
        item.gameId = match.gameId;
        item.pid = match.entry.pid;
        item.processName = match.entry.name;
        item.creationTime100ns = QueryProcessCreationTime(match.entry.pid);
        if (options_.detectWindows) {
            auto window = QueryWindowInfo(match.entry.pid);
            if (window) {
                item.windowTitle = window.Value().title;
                item.isForeground = window.Value().isForeground;
                item.isFullscreen = window.Value().isFullscreen;
            }
        }
        presence.push_back(std::move(item));
    }

    DiffResult diff = DiffProcessPresence(tracked_, presence, nextGeneration_);
    tracked_ = std::move(diff.tracked);
    return common::Result<std::vector<ProcessTransition>>::Success(
        std::move(diff.transitions));
}

void ProcessWatcher::WorkerLoop() noexcept {
    for (;;) {
        std::vector<ProcessTransition> transitions;
        auto result = PollOnce();
        if (result) {
            transitions = std::move(result.Value());
        }
        // 单轮失败跳过：观测器不因一次枚举失败而退出。

        std::vector<ProcessEventCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_) {
                break;
            }
            callbacks = callbacks_;
        }

        // 锁外调用回调（黄色不变量：回调不持锁）。
        for (const auto& transition : transitions) {
            for (const auto& callback : callbacks) {
                try {
                    callback(transition);
                } catch (...) {
                    // 回调抛异常不得终止轮询线程。
                }
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        stopCv_.wait_for(lock, options_.pollInterval,
                         [this] { return stopRequested_; });
        if (stopRequested_) {
            break;
        }
    }
}

} // namespace optimizer::process
