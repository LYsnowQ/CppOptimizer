#include "process/process_watcher.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using optimizer::config::GameConfig;
namespace process = optimizer::process;

GameConfig MakeGame(std::string id, std::vector<std::string> names) {
    GameConfig game;
    game.id = std::move(id);
    game.processNames = std::move(names);
    return game;
}

// 当前测试进程的可执行文件名（如 L"CppOptimizerProcessTests.exe"）。
std::wstring CurrentProcessExeName() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0) {
        return L"";
    }
    const std::wstring path(buffer, length);
    const auto pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

// 测试进程名假定为 ASCII：显式宽转窄，避免隐式窄化警告。
std::string NarrowAscii(const std::wstring& wide) {
    std::string narrow;
    narrow.reserve(wide.size());
    for (const wchar_t ch : wide) {
        narrow.push_back(static_cast<char>(ch));
    }
    return narrow;
}

process::GamePresence MakePresence(std::string gameId, std::uint32_t pid,
                                   std::wstring name = L"g.exe",
                                   std::uint64_t creation = 1000,
                                   bool foreground = false) {
    process::GamePresence presence;
    presence.gameId = std::move(gameId);
    presence.pid = pid;
    presence.processName = std::move(name);
    presence.creationTime100ns = creation;
    presence.isForeground = foreground;
    return presence;
}

process::ProcessInfo MakeInfo(std::string gameId, std::uint32_t pid,
                              process::ProcessState state,
                              std::uint64_t generation,
                              std::uint64_t creation = 1000) {
    process::ProcessInfo info;
    info.gameId = std::move(gameId);
    info.pid = pid;
    info.state = state;
    info.generation = generation;
    info.creationTime100ns = creation;
    return info;
}

// ---------- 纯函数测试 ----------

bool TestStateToString() {
    return std::wstring(process::StateToString(process::ProcessState::NotRunning)) ==
               L"NotRunning" &&
           std::wstring(process::StateToString(process::ProcessState::Starting)) ==
               L"Starting" &&
           std::wstring(process::StateToString(process::ProcessState::Running)) ==
               L"Running" &&
           std::wstring(process::StateToString(process::ProcessState::Suspended)) ==
               L"Suspended" &&
           std::wstring(process::StateToString(process::ProcessState::Exiting)) ==
               L"Exiting";
}

bool TestProcessNameMatches() {
    return process::ProcessNameMatches(L"Game.exe", L"game.EXE") &&
           process::ProcessNameMatches(L"Game.exe", L"Game.exe") &&
           process::ProcessNameMatches(L"", L"x") == false &&
           process::ProcessNameMatches(L"Game.exe", L"Game") == false &&
           process::ProcessNameMatches(L"Game.exe", L"Other.exe") == false &&
           process::ProcessNameMatches(L"Game", L"Game.exe") == false;
}

bool TestMatchRulesToEntries() {
    const std::vector<process::GameRule> rules = {
        {"game-a", {L"a.exe"}},
        {"game-b", {L"b.exe", L"B2.exe"}},
        {"no-match", {L"zzz.exe"}},
    };
    const std::vector<process::ProcessEntry> entries = {
        {100, L"not.exe"},
        {101, L"A.EXE"},  // 大小写不敏感命中 game-a
        {102, L"b2.exe"}, // 第二候选名命中 game-b
    };
    const auto matched = process::MatchRulesToEntries(rules, entries);
    return matched.size() == 2 &&
           matched[0].gameId == "game-a" && matched[0].entry.pid == 101 &&
           matched[1].gameId == "game-b" && matched[1].entry.pid == 102;
}

bool TestMatchRulesFirstMatchPerRule() {
    const std::vector<process::GameRule> rules = {{"one", {L"a.exe"}}};
    const std::vector<process::ProcessEntry> entries = {
        {101, L"a.exe"}, {102, L"a.exe"}};
    const auto matched = process::MatchRulesToEntries(rules, entries);
    return matched.size() == 1 && matched[0].entry.pid == 101;
}

bool TestMatchRulesTitleFilterSelectsInstance() {
    // 同名多实例：首个进程名命中但标题不满足，应继续找下一个标题满足的实例。
    const std::vector<process::GameRule> rules = {
        {"g", {L"Game.exe"}, L"Game Window"}};
    const std::vector<process::ProcessEntry> entries = {
        {100, L"Game.exe", L"Game Launcher", true},
        {101, L"Game.exe", L"Game Window", true},
    };
    const auto matched = process::MatchRulesToEntries(rules, entries);
    return matched.size() == 1 && matched[0].entry.pid == 101;
}

bool TestMatchRulesTitleFilterCaseInsensitiveAndChinese() {
    { // ASCII 大小写不敏感子串
        const std::vector<process::GameRule> rules = {
            {"g", {L"Game.exe"}, L"client"}};
        const std::vector<process::ProcessEntry> entries = {
            {100, L"Game.exe", L"GAME CLIENT 1.0", true}};
        const auto matched = process::MatchRulesToEntries(rules, entries);
        if (matched.size() != 1 || matched[0].entry.pid != 100) {
            return false;
        }
    }
    { // 中文子串
        const std::vector<process::GameRule> rules = {
            {"g", {L"Game.exe"}, L"原神"}};
        const std::vector<process::ProcessEntry> entries = {
            {100, L"Game.exe", L"原神启动器", true},
            {101, L"Game.exe", L"崩坏启动器", true}};
        const auto matched = process::MatchRulesToEntries(rules, entries);
        if (matched.size() != 1 || matched[0].entry.pid != 100) {
            return false;
        }
    }
    return true;
}

bool TestMatchRulesTitleFilterUnknownOrNoWindowFails() {
    // 标题未查询（EnumWindows 未跑/失败）与已查询但无可见窗口（标题为空）：
    // 均不满足标题过滤，规则视为无命中（保守，不把标题不符的进程当游戏）。
    const std::vector<process::GameRule> rules = {
        {"g", {L"Game.exe"}, L"Game"}};
    const std::vector<process::ProcessEntry> entries = {
        {100, L"Game.exe", L"", false},
        {101, L"Game.exe", L"", true},
    };
    const auto matched = process::MatchRulesToEntries(rules, entries);
    return matched.empty();
}

bool TestMatchRulesEmptyTitleFilterNoFilter() {
    // 空标题过滤 = 仅按进程名匹配（既有语义），标题是否已知/匹配不参与选择。
    const std::vector<process::GameRule> rules = {{"one", {L"a.exe"}}};
    const std::vector<process::ProcessEntry> entries = {
        {101, L"a.exe", L"Launcher", true},
        {102, L"a.exe", L"Game Window", false}};
    const auto matched = process::MatchRulesToEntries(rules, entries);
    return matched.size() == 1 && matched[0].entry.pid == 101;
}

bool TestBuildGameRules() {
    std::vector<GameConfig> games;
    games.push_back(MakeGame("g", {"Example.exe", "辅助.exe"}));
    auto rules = process::BuildGameRules(games);
    return rules.HasValue() && rules.Value().size() == 1 &&
           rules.Value()[0].id == "g" &&
           rules.Value()[0].processNames.size() == 2 &&
           rules.Value()[0].processNames[0] == L"Example.exe" &&
           rules.Value()[0].processNames[1] == L"辅助.exe";
}

bool TestBuildGameRulesInvalidUtf8() {
    std::vector<GameConfig> games;
    games.push_back(MakeGame("bad", {std::string("\xC3\x28", 2)})); // 非法 UTF-8
    auto rules = process::BuildGameRules(games);
    return !rules.HasValue();
}

bool TestBuildGameRulesCarriesTitleFilter() {
    std::vector<GameConfig> games;
    games.push_back(MakeGame("g", {"Example.exe"}));
    games[0].windowTitleContains = "辅助";
    auto rules = process::BuildGameRules(games);
    return rules.HasValue() && rules.Value().size() == 1 &&
           rules.Value()[0].windowTitleContains == L"辅助";
}

bool TestBuildGameRulesRejectsInvalidTitleUtf8() {
    std::vector<GameConfig> games;
    games.push_back(MakeGame("bad", {"ok.exe"}));
    games[0].windowTitleContains = std::string("\xC3\x28", 2); // 非法 UTF-8
    auto rules = process::BuildGameRules(games);
    return !rules.HasValue();
}

// ---------- 状态差分测试 ----------

bool TestDiffEmptyToEmpty() {
    std::uint64_t generation = 1;
    auto result = process::DiffProcessPresence({}, {}, generation);
    return result.tracked.empty() && result.transitions.empty() && generation == 1;
}

bool TestDiffNewProcessStarts() {
    std::uint64_t generation = 1;
    const std::vector<process::GamePresence> presence = {MakePresence("g", 100)};
    auto result = process::DiffProcessPresence({}, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Starting &&
           result.tracked[0].generation == 1 &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::NotRunning &&
           result.transitions[0].current == process::ProcessState::Starting &&
           generation == 2;
}

bool TestDiffStartingToRunning() {
    std::uint64_t generation = 1;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Starting, 1)};
    const std::vector<process::GamePresence> presence = {MakePresence("g", 100)};
    auto result = process::DiffProcessPresence(previous, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Running &&
           result.tracked[0].generation == 1 &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::Starting &&
           result.transitions[0].current == process::ProcessState::Running &&
           generation == 1; // 代数不增长
}

bool TestDiffRunningStays() {
    std::uint64_t generation = 5;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Running, 2)};
    const std::vector<process::GamePresence> presence = {
        MakePresence("g", 100, L"g.exe", 1000, true)};
    auto result = process::DiffProcessPresence(previous, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Running &&
           result.tracked[0].isForeground &&
           result.transitions.empty(); // 状态未变不产生事件
}

bool TestDiffRunningExits() {
    std::uint64_t generation = 5;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Running, 2)};
    auto result = process::DiffProcessPresence(previous, {}, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Exiting &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::Running &&
           result.transitions[0].current == process::ProcessState::Exiting;
}

bool TestDiffExitingGone() {
    std::uint64_t generation = 5;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Exiting, 2)};
    auto result = process::DiffProcessPresence(previous, {}, generation);
    return result.tracked.empty() &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::Exiting &&
           result.transitions[0].current == process::ProcessState::NotRunning;
}

bool TestDiffStartingExits() {
    std::uint64_t generation = 5;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Starting, 1)};
    auto result = process::DiffProcessPresence(previous, {}, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Exiting &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::Starting &&
           result.transitions[0].current == process::ProcessState::Exiting;
}

bool TestDiffExitingReappears() {
    std::uint64_t generation = 5;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Exiting, 2)};
    const std::vector<process::GamePresence> presence = {MakePresence("g", 100)};
    auto result = process::DiffProcessPresence(previous, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Running &&
           result.tracked[0].generation == 2 &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::Exiting &&
           result.transitions[0].current == process::ProcessState::Running;
}

bool TestDiffRestart() {
    std::uint64_t generation = 1;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Running, 1, 1000)};
    // 同一 pid，创建时间不同：判定重启。
    const std::vector<process::GamePresence> presence = {
        MakePresence("g", 100, L"g.exe", 2000)};
    auto result = process::DiffProcessPresence(previous, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Starting &&
           result.tracked[0].generation == 1 &&
           result.transitions.size() == 2 &&
           result.transitions[0].previous == process::ProcessState::Running &&
           result.transitions[0].current == process::ProcessState::Exiting &&
           result.transitions[1].previous == process::ProcessState::NotRunning &&
           result.transitions[1].current == process::ProcessState::Starting &&
           generation == 2;
}

bool TestDiffRestartUnknownCreationNotRestart() {
    std::uint64_t generation = 1;
    // 上一轮创建时间未知（0）：不判定重启，视为同一生命周期。
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Running, 1, 0)};
    const std::vector<process::GamePresence> presence = {
        MakePresence("g", 100, L"g.exe", 2000)};
    auto result = process::DiffProcessPresence(previous, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Running &&
           result.tracked[0].generation == 1 &&
           result.transitions.empty() && generation == 1;
}

bool TestDiffSuspendedResumes() {
    std::uint64_t generation = 9;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("g", 100, process::ProcessState::Suspended, 3)};
    const std::vector<process::GamePresence> presence = {MakePresence("g", 100)};
    auto result = process::DiffProcessPresence(previous, presence, generation);
    return result.tracked.size() == 1 &&
           result.tracked[0].state == process::ProcessState::Running &&
           result.transitions.size() == 1 &&
           result.transitions[0].previous == process::ProcessState::Suspended &&
           result.transitions[0].current == process::ProcessState::Running;
}

bool TestDiffMultipleGamesIndependent() {
    std::uint64_t generation = 3;
    const std::vector<process::ProcessInfo> previous = {
        MakeInfo("a", 100, process::ProcessState::Running, 1),
        MakeInfo("b", 200, process::ProcessState::Starting, 2),
    };
    const std::vector<process::GamePresence> presence = {
        MakePresence("a", 100), // 继续运行
        MakePresence("c", 300), // 新游戏
    };
    auto result = process::DiffProcessPresence(previous, presence, generation);
    // a 在场 Running；b 缺席 -> Exiting；c 新 -> Starting。
    bool statesOk = false;
    for (const auto& info : result.tracked) {
        if (info.gameId == "a" && info.state == process::ProcessState::Running) {
            statesOk = true;
        }
    }
    return result.tracked.size() == 3 && generation == 4 && statesOk &&
           result.transitions.size() == 2;
}

bool TestDiffSamePidTwoGames() {
    // 同一 pid 命中两个游戏规则：各自独立跟踪。
    std::uint64_t generation = 1;
    const std::vector<process::GamePresence> presence = {
        MakePresence("a", 100),
        MakePresence("b", 100),
    };
    auto result = process::DiffProcessPresence({}, presence, generation);
    return result.tracked.size() == 2 && generation == 3 &&
           result.tracked[0].gameId != result.tracked[1].gameId &&
           result.transitions.size() == 2;
}

// ---------- Windows 只读封装测试 ----------

bool TestEnumerateProcessesContainsSelf() {
    auto result = process::EnumerateProcesses();
    if (!result.HasValue()) {
        return false;
    }
    const DWORD self = ::GetCurrentProcessId();
    for (const auto& entry : result.Value()) {
        if (entry.pid == self) {
            return true;
        }
    }
    return false;
}

bool TestQueryCreationTimeSelf() {
    return process::QueryProcessCreationTime(::GetCurrentProcessId()) != 0;
}

bool TestQueryWindowInfoSelf() {
    // 测试进程无可见顶层窗口：标题为空、非前台（防御性宽松断言）。
    auto result = process::QueryWindowInfo(::GetCurrentProcessId());
    return result.HasValue() && !result.Value().isForeground;
}

// ---------- 观测器集成测试 ----------

bool TestWatcherPollOnceSelfRule() {
    // 以当前测试进程自身为规则，验证 枚举 -> 匹配 -> 差分 全链路。
    const std::wstring exeName = CurrentProcessExeName();
    if (exeName.empty()) {
        return false;
    }
    const std::string narrow = NarrowAscii(exeName);

    std::vector<GameConfig> games;
    games.push_back(MakeGame("self", {narrow}));

    process::ProcessWatcher watcher;
    auto setup = watcher.SetRules(games);
    if (!setup.HasValue()) {
        return false;
    }

    auto poll1 = watcher.PollOnce();
    if (!poll1.HasValue()) {
        return false;
    }
    const auto tracked1 = watcher.GetTrackedProcesses();
    if (tracked1.size() != 1 || tracked1[0].gameId != "self" ||
        tracked1[0].pid != ::GetCurrentProcessId() ||
        tracked1[0].state != process::ProcessState::Starting ||
        tracked1[0].creationTime100ns == 0) {
        return false;
    }

    auto poll2 = watcher.PollOnce();
    if (!poll2.HasValue()) {
        return false;
    }
    const auto tracked2 = watcher.GetTrackedProcesses();
    return tracked2.size() == 1 &&
           tracked2[0].state == process::ProcessState::Running &&
           tracked2[0].generation == tracked1[0].generation;
}

bool TestWatcherThreadDeliversEvents() {
    const std::wstring exeName = CurrentProcessExeName();
    if (exeName.empty()) {
        return false;
    }
    const std::string narrow = NarrowAscii(exeName);

    std::vector<GameConfig> games;
    games.push_back(MakeGame("self", {narrow}));

    process::ProcessWatcher watcher(process::ProcessWatcher::Options{
        std::chrono::milliseconds(200), /* detectWindows */ false});
    auto setup = watcher.SetRules(games);
    if (!setup.HasValue()) {
        return false;
    }

    std::promise<void> started;
    std::atomic<bool> sawStarting{false};
    std::atomic<bool> sawRunning{false};
    watcher.Subscribe([&](const process::ProcessTransition& transition) {
        if (transition.gameId != "self") {
            return;
        }
        if (transition.current == process::ProcessState::Starting &&
            !sawStarting.exchange(true)) {
            started.set_value();
        }
        if (transition.current == process::ProcessState::Running) {
            sawRunning.store(true);
        }
    });

    auto startResult = watcher.Start();
    if (!startResult.HasValue()) {
        return false;
    }

    const bool gotStarting =
        started.get_future().wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    // 留足时间让第二轮轮询完成 Starting -> Running 转移。
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    watcher.Stop();
    return gotStarting && sawStarting.load() && sawRunning.load();
}

bool TestWatcherStopIdempotent() {
    process::ProcessWatcher watcher;
    watcher.Stop(); // 未启动时停止：幂等
    return !watcher.IsRunning();
}

// ---------- 进程目录测试 ----------

bool TestQueryProcessDetailsSelf() {
    const auto result = process::QueryProcessDetails(::GetCurrentProcessId());
    if (!result.HasValue()) {
        return false;
    }
    const auto& d = result.Value();
    // 测试进程：名称为 CppOptimizerProcessTests.exe，路径以同名结尾，内存非零。
    return d.pid == ::GetCurrentProcessId() &&
           !d.name.empty() &&
           !d.executablePath.empty() &&
           d.executablePath.size() >= d.name.size() &&
           d.executablePath.compare(d.executablePath.size() - d.name.size(),
                                    d.name.size(), d.name) == 0 &&
           d.workingSetBytes > 0;
}

bool TestQueryProcessDetailsMissingPid() {
    // 不存在的 pid：应失败（Win32 域）。
    const auto result = process::QueryProcessDetails(0xFFFFFFFE);
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Win32;
}

bool TestEnumerateProcessDetailsAllContainsSelf() {
    auto result = process::EnumerateProcessDetails(false);
    if (!result.HasValue()) {
        return false;
    }
    bool foundSelf = false;
    for (const auto& item : result.Value()) {
        if (item.pid == ::GetCurrentProcessId()) {
            foundSelf = true;
            break;
        }
    }
    return foundSelf;
}

bool TestEnumerateProcessDetailsWindowOnlyExcludesSelf() {
    // 测试进程无可见窗口：windowOnly 列表不应包含自身。
    auto result = process::EnumerateProcessDetails(true);
    if (!result.HasValue()) {
        return false;
    }
    for (const auto& item : result.Value()) {
        if (item.pid == ::GetCurrentProcessId()) {
            return false;
        }
    }
    return true;
}

bool TestProcessMatchesFilter() {
    process::ProcessDetails details;
    details.name = L"ExampleGame.exe";
    details.executablePath = L"C:\\Games\\ExampleGame\\ExampleGame.exe";
    details.windowTitle = L"艾尔登法环";
    return process::ProcessMatchesFilter(details, L"") &&
           process::ProcessMatchesFilter(details, L"example") &&
           process::ProcessMatchesFilter(details, L"GAMES") &&
           process::ProcessMatchesFilter(details, L"艾尔登") &&
           !process::ProcessMatchesFilter(details, L"nonexistent");
}

// ---------- 规则生成测试 ----------

bool TestDeriveGameId() {
    auto id1 = process::DeriveGameId(L"EldenRing.exe");
    auto id2 = process::DeriveGameId(L"eldenring.EXE");
    auto id3 = process::DeriveGameId(L"explorer"); // 无扩展名
    auto id4 = process::DeriveGameId(L"哔哩哔哩.exe"); // 非 ASCII 保留
    return id1.HasValue() && id1.Value() == "eldenring" &&
           id2.HasValue() && id2.Value() == "eldenring" &&
           id3.HasValue() && id3.Value() == "explorer" &&
           id4.HasValue() && id4.Value() == "哔哩哔哩";
}

bool TestMakeUniqueGameId() {
    const std::vector<optimizer::config::GameConfig> existing = {
        MakeGame("eldenring", {}),
        MakeGame("Game-2", {}),
    };
    // 保留传入大小写（派生函数 DeriveGameId 负责小写化）；仅处理冲突。
    return process::MakeUniqueGameId("new", existing) == "new" &&
           process::MakeUniqueGameId("ELDENRING", existing) == "ELDENRING-2" &&
           process::MakeUniqueGameId("game-2", existing) == "game-2-2";
}

bool TestBuildGameRuleFromProcess() {
    process::ProcessDetails details;
    details.name = L"EldenRing.exe";
    details.windowTitle = L"艾尔登法环";
    details.pid = 1234;

    // 无冲突：id 派生 + display_name 取窗口标题。
    auto rule = process::BuildGameRuleFromProcess(details, {});
    if (!rule.HasValue() || rule.Value().id != "eldenring" ||
        rule.Value().displayName != "艾尔登法环" ||
        rule.Value().processNames.size() != 1 ||
        rule.Value().processNames[0] != "EldenRing.exe") {
        return false;
    }

    // 冲突：加后缀；hint 优先于窗口标题。
    const std::vector<optimizer::config::GameConfig> existing = {
        MakeGame("eldenring", {})};
    auto rule2 = process::BuildGameRuleFromProcess(details, existing, L"自定名");
    return rule2.HasValue() && rule2.Value().id == "eldenring-2" &&
           rule2.Value().displayName == "自定名";
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

    run(L"StateToString names", &TestStateToString);
    run(L"ProcessNameMatches case-insensitive", &TestProcessNameMatches);
    run(L"MatchRulesToEntries rules ordered", &TestMatchRulesToEntries);
    run(L"MatchRulesToEntries first match per rule", &TestMatchRulesFirstMatchPerRule);
    run(L"MatchRules title filter selects instance", &TestMatchRulesTitleFilterSelectsInstance);
    run(L"MatchRules title filter case-insensitive/chinese", &TestMatchRulesTitleFilterCaseInsensitiveAndChinese);
    run(L"MatchRules title filter unknown/no-window fails", &TestMatchRulesTitleFilterUnknownOrNoWindowFails);
    run(L"MatchRules empty title filter = no filter", &TestMatchRulesEmptyTitleFilterNoFilter);
    run(L"BuildGameRules UTF-8 to wide", &TestBuildGameRules);
    run(L"BuildGameRules rejects invalid UTF-8", &TestBuildGameRulesInvalidUtf8);
    run(L"BuildGameRules carries title filter", &TestBuildGameRulesCarriesTitleFilter);
    run(L"BuildGameRules rejects invalid title UTF-8", &TestBuildGameRulesRejectsInvalidTitleUtf8);
    run(L"Diff empty to empty", &TestDiffEmptyToEmpty);
    run(L"Diff new process starts", &TestDiffNewProcessStarts);
    run(L"Diff starting to running", &TestDiffStartingToRunning);
    run(L"Diff running stays silent", &TestDiffRunningStays);
    run(L"Diff running exits", &TestDiffRunningExits);
    run(L"Diff exiting gone", &TestDiffExitingGone);
    run(L"Diff starting exits", &TestDiffStartingExits);
    run(L"Diff exiting reappears", &TestDiffExitingReappears);
    run(L"Diff restart by creation time", &TestDiffRestart);
    run(L"Diff unknown creation not restart", &TestDiffRestartUnknownCreationNotRestart);
    run(L"Diff suspended resumes", &TestDiffSuspendedResumes);
    run(L"Diff multiple games independent", &TestDiffMultipleGamesIndependent);
    run(L"Diff same pid two games", &TestDiffSamePidTwoGames);
    run(L"EnumerateProcesses contains self", &TestEnumerateProcessesContainsSelf);
    run(L"QueryProcessCreationTime self", &TestQueryCreationTimeSelf);
    run(L"QueryWindowInfo self", &TestQueryWindowInfoSelf);
    run(L"QueryProcessDetails self", &TestQueryProcessDetailsSelf);
    run(L"QueryProcessDetails missing pid fails", &TestQueryProcessDetailsMissingPid);
    run(L"EnumerateProcessDetails all contains self", &TestEnumerateProcessDetailsAllContainsSelf);
    run(L"EnumerateProcessDetails windowOnly excludes self", &TestEnumerateProcessDetailsWindowOnlyExcludesSelf);
    run(L"ProcessMatchesFilter substring", &TestProcessMatchesFilter);
    run(L"DeriveGameId strips extension and folds case", &TestDeriveGameId);
    run(L"MakeUniqueGameId appends suffix on conflict", &TestMakeUniqueGameId);
    run(L"BuildGameRuleFromProcess derives rule", &TestBuildGameRuleFromProcess);
    run(L"Watcher PollOnce self rule", &TestWatcherPollOnceSelfRule);
    run(L"Watcher thread delivers events", &TestWatcherThreadDeliversEvents);
    run(L"Watcher stop idempotent", &TestWatcherStopIdempotent);
    return failed == 0 ? 0 : 1;
}
