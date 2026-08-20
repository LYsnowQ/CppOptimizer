#include "config/config_manager.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

optimizer::config::GameConfig MakeGame(std::string id, std::vector<std::string> names) {
    optimizer::config::GameConfig game;
    game.id = std::move(id);
    game.processNames = std::move(names);
    return game;
}

std::wstring MakeTempConfigPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        return L"config_test.toml";
    }
    return std::wstring(buffer) +
           L"cppoptimizer_config_test_" + std::to_wstring(::GetCurrentProcessId()) +
           L".toml";
}

bool WriteTempConfig(const std::wstring& path, const std::string& content) {
    std::ofstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        return false;
    }
    stream << content;
    return true;
}

bool TestParseRunMode() {
    return optimizer::config::ParseRunMode("observe").HasValue() &&
           optimizer::config::ParseRunMode("BALANCED").HasValue() &&
           optimizer::config::ParseRunMode("Experimental").HasValue() &&
           !optimizer::config::ParseRunMode("hack").HasValue();
}

bool TestParseCleanLevel() {
    return optimizer::config::ParseCleanLevel("none").HasValue() &&
           optimizer::config::ParseCleanLevel("LIGHT").HasValue() &&
           !optimizer::config::ParseCleanLevel("full").HasValue();
}

bool TestLoadConfigDefaults() {
    // 空表：所有键缺失，应使用默认值。
    const std::wstring path = MakeTempConfigPath();
    if (!WriteTempConfig(path, "version = 1\n")) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    if (!result.HasValue()) {
        return false;
    }
    const auto& c = result.Value();
    return c.version == 1 &&
           c.application.mode == optimizer::config::RunMode::Observe &&
           c.application.safeModeOnRecoveryError &&
           c.logging.level == "info" &&
           c.logging.maxFileMb == 10 &&
           c.logging.maxFiles == 5 &&
           c.memory.queryEnabled &&
           !c.memory.scheduledCleanEnabled &&
           !c.memory.allowNativeWrite &&
           c.memory.maxCleanLevel == optimizer::config::CleanLevel::Light;
}

bool TestLoadConfigFull() {
    const std::wstring path = MakeTempConfigPath();
    const std::string content = R"(
version = 2

[application]
mode = "balanced"
safe_mode_on_recovery_error = false

[logging]
level = "debug"
directory = "C:\\logs"
max_file_mb = 50
max_files = 3
console = false

[memory]
query_enabled = true
scheduled_clean_enabled = true
allow_native_write = false
max_clean_level = "light"
)";
    if (!WriteTempConfig(path, content)) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    if (!result.HasValue()) {
        return false;
    }
    const auto& c = result.Value();
    return c.version == 2 &&
           c.application.mode == optimizer::config::RunMode::Balanced &&
           !c.application.safeModeOnRecoveryError &&
           c.logging.level == "debug" &&
           c.logging.directory == "C:\\logs" &&
           c.logging.maxFileMb == 50 &&
           c.logging.maxFiles == 3 &&
           !c.logging.console &&
           c.memory.scheduledCleanEnabled &&
           !c.memory.allowNativeWrite;
}

bool TestLoadConfigSyntaxError() {
    const std::wstring path = MakeTempConfigPath();
    if (!WriteTempConfig(path, "version = \n[[ broken")) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestLoadConfigMissingFile() {
    auto result = optimizer::config::LoadConfig(L"Z:\\nonexistent\\path\\no.toml");
    return !result.HasValue();
}

bool TestLoadConfigBadModeFails() {
    const std::wstring path = MakeTempConfigPath();
    if (!WriteTempConfig(path, "[application]\nmode = \"turbo\"\n")) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    // 非法运行模式是显式配置错误，应报告失败。
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestLoadConfigBadCleanLevelDefaults() {
    const std::wstring path = MakeTempConfigPath();
    if (!WriteTempConfig(path, "[memory]\nmax_clean_level = \"full\"\n")) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    // 非法清理级别失败安全：保持默认 Light，不视为致命。
    return result.HasValue() &&
           result.Value().memory.maxCleanLevel == optimizer::config::CleanLevel::Light;
}

bool TestLoadConfigChineseValues() {
    // 配置值可含中文，UTF-8 往返无损。
    const std::wstring path = MakeTempConfigPath();
    const std::string content =
        "[logging]\ndirectory = \"日志目录\"\n\n[[games]]\nid = \"example\"\n";
    if (!WriteTempConfig(path, content)) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    return result.HasValue() &&
           result.Value().logging.directory == "日志目录";
}

bool TestParsePriorityLevel() {
    return optimizer::config::ParsePriorityLevel("none").HasValue() &&
           optimizer::config::ParsePriorityLevel("ABOVE_NORMAL").HasValue() &&
           optimizer::config::ParsePriorityLevel("High").HasValue() &&
           !optimizer::config::ParsePriorityLevel("realtime").HasValue() &&
           !optimizer::config::ParsePriorityLevel("max").HasValue();
}

bool TestLoadConfigExtendedSections() {
    // 覆盖新增节：layers/power/priority/gpu_heartbeat/scheduler/disk_cache。
    const std::wstring path = MakeTempConfigPath();
    const std::string content = R"(
[layers]
monitoring = false
maintenance = true
emergency = true

[power]
execution_required = false
display_required = true
switch_power_scheme = true

[priority]
enabled = true
max_level = "high"

[gpu_heartbeat]
enabled = true
max_measured_load_percent = 0.75

[scheduler]
enabled = true

[disk_cache]
enabled = true
)";
    if (!WriteTempConfig(path, content)) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    if (!result.HasValue()) {
        return false;
    }
    const auto& c = result.Value();
    return !c.layers.monitoring && c.layers.maintenance && c.layers.emergency &&
           !c.power.executionRequired && c.power.displayRequired &&
           c.power.switchPowerScheme &&
           c.priority.enabled &&
           c.priority.maxLevel == optimizer::config::PriorityLevel::High &&
           c.gpuHeartbeat.enabled &&
           c.gpuHeartbeat.maxMeasuredLoadPercent == 0.75 &&
           c.scheduler.enabled && c.diskCache.enabled;
}

bool TestLoadConfigGamesArray() {
    const std::wstring path = MakeTempConfigPath();
    const std::string content = R"(
[[games]]
id = "game-a"
display_name = "Game A"
process_names = ["A.exe", "A_launcher.exe"]
pause_when_background = false

[[games]]
id = "game-b"
display_name = "Game B"
process_names = ["B.exe"]
)";
    if (!WriteTempConfig(path, content)) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    if (!result.HasValue()) {
        return false;
    }
    const auto& c = result.Value();
    return c.games.size() == 2 &&
           c.games[0].id == "game-a" &&
           c.games[0].displayName == "Game A" &&
           c.games[0].processNames.size() == 2 &&
           c.games[0].processNames[1] == "A_launcher.exe" &&
           !c.games[0].pauseWhenBackground &&
           c.games[1].id == "game-b" &&
           c.games[1].processNames.size() == 1 &&
           c.games[1].pauseWhenBackground; // 缺省值 true
}

bool TestLoadConfigGameWithoutIdSkipped() {
    // 缺 id 的 game 条目跳过。
    const std::wstring path = MakeTempConfigPath();
    const std::string content = R"(
[[games]]
display_name = "No Id"

[[games]]
id = "ok"
)";
    if (!WriteTempConfig(path, content)) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    return result.HasValue() && result.Value().games.size() == 1 &&
           result.Value().games[0].id == "ok";
}

// ---------- 规则写入与合并测试 ----------

bool TestToTomlString() {
    return optimizer::config::ToTomlString("a\"b\\c") == "\"a\\\"b\\\\c\"" &&
           optimizer::config::ToTomlString("中文标题") == "\"中文标题\"" &&
           optimizer::config::ToTomlString("") == "\"\"";
}

bool TestFormatGameRulesRoundTrip() {
    // 生成文本 -> 写临时文件 -> LoadConfig 读回，字段一致。
    const std::wstring path = MakeTempConfigPath();
    std::vector<optimizer::config::GameConfig> rules;
    optimizer::config::GameConfig game;
    game.id = "eldenring";
    game.displayName = "艾尔登法环";
    game.processNames.push_back("EldenRing.exe");
    game.pauseWhenBackground = true;
    rules.push_back(game);

    const std::string content =
        "version = 1\n" + optimizer::config::FormatGameRulesToml(rules);
    if (!WriteTempConfig(path, content)) {
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    if (!result.HasValue() || result.Value().games.size() != 1) {
        return false;
    }
    const auto& g = result.Value().games[0];
    return g.id == "eldenring" && g.displayName == "艾尔登法环" &&
           g.processNames.size() == 1 && g.processNames[0] == "EldenRing.exe" &&
           g.pauseWhenBackground;
}

bool TestAppendGameRulesKeepsOriginal() {
    // 追加不重写原内容：注释保留，version 不变，规则可读回。
    const std::wstring path = MakeTempConfigPath();
    if (!WriteTempConfig(path, "# 我的注释\nversion = 1\n")) {
        return false;
    }
    std::vector<optimizer::config::GameConfig> rules;
    optimizer::config::GameConfig game;
    game.id = "added";
    game.processNames.push_back("Added.exe");
    rules.push_back(game);

    auto append = optimizer::config::AppendGameRules(path, rules);
    if (!append.HasValue()) {
        std::filesystem::remove(std::filesystem::path(path));
        return false;
    }
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    if (!result.HasValue()) {
        return false;
    }
    const auto& c = result.Value();
    return c.version == 1 && c.games.size() == 1 && c.games[0].id == "added";
}

bool TestAppendGameRulesCreatesFile() {
    // 文件不存在：创建并可读回。
    const std::wstring path =
        MakeTempConfigPath() + L"_new";
    std::filesystem::remove(std::filesystem::path(path));
    std::vector<optimizer::config::GameConfig> rules;
    optimizer::config::GameConfig game;
    game.id = "created";
    game.processNames.push_back("Created.exe");
    rules.push_back(game);

    auto append = optimizer::config::AppendGameRules(path, rules);
    const bool appendOk = append.HasValue();
    auto result = optimizer::config::LoadConfig(path);
    std::filesystem::remove(std::filesystem::path(path));
    return appendOk && result.HasValue() && result.Value().games.size() == 1 &&
           result.Value().games[0].id == "created";
}

bool TestMergeGameRules() {
    std::vector<optimizer::config::GameConfig> mainRules;
    mainRules.push_back(MakeGame("a", {}));
    mainRules.push_back(MakeGame("b", {}));

    std::vector<optimizer::config::GameConfig> localRules;
    optimizer::config::GameConfig overwrite;
    overwrite.id = "A"; // 大小写不敏感覆盖 main 的 "a"
    overwrite.displayName = "覆盖";
    localRules.push_back(overwrite);
    localRules.push_back(MakeGame("c", {}));

    const auto merged = optimizer::config::MergeGameRules(mainRules, localRules);
    // "A" 覆盖 main 的 "a"（id 与 displayName 均以 local 为准），"c" 追加。
    return merged.size() == 3 && merged[0].id == "A" &&
           merged[0].displayName == "覆盖" && merged[1].id == "b" &&
           merged[2].id == "c";
}

bool TestLoadConfigWithLocal() {
    const std::wstring mainPath = MakeTempConfigPath() + L"_main";
    const std::wstring localPath = MakeTempConfigPath() + L"_local";
    if (!WriteTempConfig(mainPath, "version = 1\n[[games]]\nid = \"main-game\"\n") ||
        !WriteTempConfig(localPath, "[[games]]\nid = \"main-game\"\ndisplay_name = \"本地覆盖\"\n[[games]]\nid = \"local-game\"\n")) {
        return false;
    }

    auto result = optimizer::config::LoadConfigWithLocal(mainPath, localPath);
    std::filesystem::remove(std::filesystem::path(mainPath));
    std::filesystem::remove(std::filesystem::path(localPath));
    if (!result.HasValue()) {
        return false;
    }
    const auto& games = result.Value().games;
    // main-game 被 local 覆盖（displayName 生效），local-game 追加。
    return games.size() == 2 && games[0].id == "main-game" &&
           games[0].displayName == "本地覆盖" && games[1].id == "local-game";
}

bool TestLoadConfigWithLocalMissingLocal() {
    // local 文件不存在：返回 main 结果，不报错。
    const std::wstring mainPath = MakeTempConfigPath() + L"_m";
    if (!WriteTempConfig(mainPath, "version = 1\n")) {
        return false;
    }
    auto result =
        optimizer::config::LoadConfigWithLocal(mainPath, L"Z:\\nonexistent\\local.toml");
    std::filesystem::remove(std::filesystem::path(mainPath));
    return result.HasValue() && result.Value().version == 1;
}

bool TestLoadConfigWithLocalBrokenLocal() {
    // local 解析失败：报错（显式路径下 local 损坏必须暴露）。
    const std::wstring mainPath = MakeTempConfigPath() + L"_m";
    const std::wstring localPath = MakeTempConfigPath() + L"_l";
    if (!WriteTempConfig(mainPath, "version = 1\n") ||
        !WriteTempConfig(localPath, "[[ broken")) {
        return false;
    }
    auto result = optimizer::config::LoadConfigWithLocal(mainPath, localPath);
    std::filesystem::remove(std::filesystem::path(mainPath));
    std::filesystem::remove(std::filesystem::path(localPath));
    return !result.HasValue();
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

    run(L"Parse run mode", &TestParseRunMode);
    run(L"Parse clean level", &TestParseCleanLevel);
    run(L"Load config uses defaults for missing keys", &TestLoadConfigDefaults);
    run(L"Load config full sections", &TestLoadConfigFull);
    run(L"Load config rejects syntax error", &TestLoadConfigSyntaxError);
    run(L"Load config rejects missing file", &TestLoadConfigMissingFile);
    run(L"Load config rejects bad mode", &TestLoadConfigBadModeFails);
    run(L"Load config defaults bad clean level", &TestLoadConfigBadCleanLevelDefaults);
    run(L"Load config keeps Chinese values", &TestLoadConfigChineseValues);
    run(L"Parse priority level", &TestParsePriorityLevel);
    run(L"Load config extended sections", &TestLoadConfigExtendedSections);
    run(L"Load config games array", &TestLoadConfigGamesArray);
    run(L"Load config skips game without id", &TestLoadConfigGameWithoutIdSkipped);
    run(L"ToTomlString escapes", &TestToTomlString);
    run(L"FormatGameRulesToml round-trip", &TestFormatGameRulesRoundTrip);
    run(L"AppendGameRules keeps original", &TestAppendGameRulesKeepsOriginal);
    run(L"AppendGameRules creates file", &TestAppendGameRulesCreatesFile);
    run(L"MergeGameRules local overrides by id", &TestMergeGameRules);
    run(L"LoadConfigWithLocal merges games", &TestLoadConfigWithLocal);
    run(L"LoadConfigWithLocal missing local", &TestLoadConfigWithLocalMissingLocal);
    run(L"LoadConfigWithLocal broken local fails", &TestLoadConfigWithLocalBrokenLocal);
    return failed == 0 ? 0 : 1;
}
