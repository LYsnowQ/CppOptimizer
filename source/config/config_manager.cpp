#include "config/config_manager.hpp"

#include "common/unique_resource.hpp"

#include <toml++/toml.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

namespace optimizer::config {

namespace {

std::string ToLower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return result;
}

// toml++ 抛 toml::parse_error；此处统一转为 common::Error Validation 域，
// 契约保证 LoadConfig 不抛异常。
common::Error TomlFailure(const toml::parse_error& error) {
    return common::Error::Validation(
        "LoadConfig", std::wstring(error.description().begin(),
                                   error.description().end()));
}

} // namespace

common::Result<RunMode> ParseRunMode(std::string_view name) {
    const std::string lower = ToLower(name);
    if (lower == "observe") {
        return common::Result<RunMode>::Success(RunMode::Observe);
    }
    if (lower == "balanced") {
        return common::Result<RunMode>::Success(RunMode::Balanced);
    }
    if (lower == "experimental") {
        return common::Result<RunMode>::Success(RunMode::Experimental);
    }
    return common::Result<RunMode>::Failure(common::Error::Validation(
        "ParseRunMode", L"Unknown run mode (expected observe/balanced/experimental)"));
}

common::Result<CleanLevel> ParseCleanLevel(std::string_view name) {
    const std::string lower = ToLower(name);
    if (lower == "none") {
        return common::Result<CleanLevel>::Success(CleanLevel::None);
    }
    if (lower == "light") {
        return common::Result<CleanLevel>::Success(CleanLevel::Light);
    }
    return common::Result<CleanLevel>::Failure(common::Error::Validation(
        "ParseCleanLevel", L"Unknown clean level (expected none/light)"));
}

common::Result<PriorityLevel> ParsePriorityLevel(std::string_view name) {
    const std::string lower = ToLower(name);
    if (lower == "none") {
        return common::Result<PriorityLevel>::Success(PriorityLevel::None);
    }
    if (lower == "above_normal") {
        return common::Result<PriorityLevel>::Success(PriorityLevel::AboveNormal);
    }
    if (lower == "high") {
        return common::Result<PriorityLevel>::Success(PriorityLevel::High);
    }
    // realtime 明确拒绝，不提供该枚举值。
    if (lower == "realtime") {
        return common::Result<PriorityLevel>::Failure(common::Error::Validation(
            "ParsePriorityLevel", L"realtime priority is forbidden"));
    }
    return common::Result<PriorityLevel>::Failure(common::Error::Validation(
        "ParsePriorityLevel", L"Unknown priority level (expected none/above_normal/high)"));
}

std::string ToTomlString(std::string_view text) noexcept {
    std::string out;
    out.reserve(text.size() + 8);
    out.push_back('"');
    for (const char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    // 其余控制字符：\uXXXX 转义。
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string FormatGameRulesToml(
    std::span<const GameConfig> rules) noexcept {
    std::string out;
    for (const auto& game : rules) {
        out += "# 由 CppOptimizer --add-game 生成\n";
        out += "[[games]]\n";
        out += "id = " + ToTomlString(game.id) + "\n";
        if (!game.displayName.empty()) {
            out += "display_name = " + ToTomlString(game.displayName) + "\n";
        }
        if (!game.processNames.empty()) {
            out += "process_names = [";
            for (std::size_t i = 0; i < game.processNames.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                out += ToTomlString(game.processNames[i]);
            }
            out += "]\n";
        }
        out += "pause_when_background = ";
        out += game.pauseWhenBackground ? "true" : "false";
        out += "\n\n";
    }
    return out;
}

common::Result<void> AppendGameRules(
    std::wstring_view path, std::span<const GameConfig> rules) noexcept {
    if (rules.empty()) {
        return common::Result<void>::Success();
    }
    const std::wstring pathStr(path);

    // 读现有内容（文件不存在视为空，不报错）。
    std::string content;
    {
        common::UniqueHandle file(::CreateFileW(
            pathStr.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.IsValid()) {
            bool readFailed = false;
            char buffer[4096]{};
            for (;;) {
                DWORD bytes = 0;
                if (!::ReadFile(file.Get(), buffer, sizeof(buffer), &bytes,
                                nullptr)) {
                    readFailed = true;
                    break;
                }
                if (bytes == 0) {
                    break; // EOF
                }
                content.append(buffer, bytes);
            }
            if (readFailed) {
                return common::Result<void>::Failure(
                    common::Error::FromWin32(::GetLastError(), "ReadFile"));
            }
        } else {
            const DWORD code = ::GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) {
                return common::Result<void>::Failure(
                    common::Error::FromWin32(code, "CreateFileW"));
            }
        }
    }

    // 追加到末尾；原文件未以换行结尾则补一个，避免拼接到同一行。
    if (!content.empty() && content.back() != '\n') {
        content.push_back('\n');
    }
    content += FormatGameRulesToml(rules);

    // 原子写：临时文件 + MoveFileEx 替换；任何失败清理临时文件。
    const std::wstring tempPath = pathStr + L".tmp";
    {
        common::UniqueHandle file(::CreateFileW(
            tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.IsValid()) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(::GetLastError(), "CreateFileW(tmp)"));
        }
        DWORD written = 0;
        if (!::WriteFile(file.Get(), content.data(),
                         static_cast<DWORD>(content.size()), &written, nullptr) ||
            written != content.size()) {
            const DWORD code = ::GetLastError();
            ::DeleteFileW(tempPath.c_str());
            return common::Result<void>::Failure(
                common::Error::FromWin32(code, "WriteFile"));
        }
        ::FlushFileBuffers(file.Get());
    }
    if (!::MoveFileExW(tempPath.c_str(), pathStr.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = ::GetLastError();
        ::DeleteFileW(tempPath.c_str());
        return common::Result<void>::Failure(
            common::Error::FromWin32(code, "MoveFileExW"));
    }
    return common::Result<void>::Success();
}

std::vector<GameConfig> MergeGameRules(
    std::span<const GameConfig> mainRules,
    std::span<const GameConfig> localRules) noexcept {
    std::vector<GameConfig> merged(mainRules.begin(), mainRules.end());
    for (const auto& local : localRules) {
        const std::string localLower = ToLower(local.id);
        bool replaced = false;
        for (auto& item : merged) {
            if (ToLower(item.id) == localLower) {
                item = local;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.push_back(local);
        }
    }
    return merged;
}

common::Result<ConfigSnapshot> LoadConfigWithLocal(
    std::wstring_view mainPath, std::wstring_view localPath) noexcept {
    auto mainResult = LoadConfig(mainPath);
    if (!mainResult) {
        return common::Result<ConfigSnapshot>::Failure(mainResult.ErrorValue());
    }

    // local 文件不存在：仅返回 main 结果（显式路径下未创建 local 不算错误）。
    std::error_code existsError;
    if (!std::filesystem::exists(std::filesystem::path(localPath),
                                 existsError)) {
        return common::Result<ConfigSnapshot>::Success(
            std::move(mainResult.Value()));
    }

    auto localResult = LoadConfig(localPath);
    if (!localResult) {
        return common::Result<ConfigSnapshot>::Failure(
            localResult.ErrorValue());
    }

    ConfigSnapshot snapshot = std::move(mainResult.Value());
    snapshot.games = MergeGameRules(snapshot.games, localResult.Value().games);
    return common::Result<ConfigSnapshot>::Success(std::move(snapshot));
}

common::Result<ConfigSnapshot> LoadConfig(std::wstring_view path) {
    // toml++ 的 parse_file 接受 UTF-8 路径；从宽路径转换。
    std::filesystem::path fsPath(path);
    toml::table table;
    try {
        table = toml::parse_file(fsPath.string());
    } catch (const toml::parse_error& error) {
        return common::Result<ConfigSnapshot>::Failure(TomlFailure(error));
    }

    ConfigSnapshot snapshot;

    if (const auto version = table["version"].value<std::int64_t>()) {
        snapshot.version = *version;
    }

    // [application]
    if (auto* section = table["application"].as_table()) {
        if (const auto mode = (*section)["mode"].value<std::string>()) {
            auto parsed = ParseRunMode(*mode);
            if (!parsed) {
                return common::Result<ConfigSnapshot>::Failure(parsed.ErrorValue());
            }
            snapshot.application.mode = parsed.Value();
        }
        if (const auto safe = (*section)["safe_mode_on_recovery_error"].value<bool>()) {
            snapshot.application.safeModeOnRecoveryError = *safe;
        }
    }

    // [logging]
    if (auto* section = table["logging"].as_table()) {
        if (const auto level = (*section)["level"].value<std::string>()) {
            snapshot.logging.level = *level;
        }
        if (const auto dir = (*section)["directory"].value<std::string>()) {
            snapshot.logging.directory = *dir;
        }
        if (const auto mb = (*section)["max_file_mb"].value<std::int64_t>()) {
            if (*mb > 0) {
                snapshot.logging.maxFileMb = static_cast<std::uint32_t>(*mb);
            }
        }
        if (const auto files = (*section)["max_files"].value<std::int64_t>()) {
            if (*files > 0) {
                snapshot.logging.maxFiles = static_cast<std::uint32_t>(*files);
            }
        }
        if (const auto console = (*section)["console"].value<bool>()) {
            snapshot.logging.console = *console;
        }
    }

    // [memory]：危险开关失败安全，任何异常都回退 false。
    if (auto* section = table["memory"].as_table()) {
        if (const auto query = (*section)["query_enabled"].value<bool>()) {
            snapshot.memory.queryEnabled = *query;
        }
        if (const auto clean = (*section)["scheduled_clean_enabled"].value<bool>()) {
            snapshot.memory.scheduledCleanEnabled = *clean;
        }
        if (const auto write = (*section)["allow_native_write"].value<bool>()) {
            snapshot.memory.allowNativeWrite = *write;
        }
        if (const auto level = (*section)["max_clean_level"].value<std::string>()) {
            auto parsed = ParseCleanLevel(*level);
            if (parsed) {
                snapshot.memory.maxCleanLevel = parsed.Value();
            }
            // 非法清理级别保持默认，不视为致命错误。
        }
    }

    // [layers]
    if (auto* section = table["layers"].as_table()) {
        if (const auto monitoring = (*section)["monitoring"].value<bool>()) {
            snapshot.layers.monitoring = *monitoring;
        }
        if (const auto maintenance = (*section)["maintenance"].value<bool>()) {
            snapshot.layers.maintenance = *maintenance;
        }
        if (const auto emergency = (*section)["emergency"].value<bool>()) {
            snapshot.layers.emergency = *emergency;
        }
    }

    // [power]：switchPowerScheme 为 R2 危险开关，仅显式 true 才开启。
    if (auto* section = table["power"].as_table()) {
        if (const auto execution = (*section)["execution_required"].value<bool>()) {
            snapshot.power.executionRequired = *execution;
        }
        if (const auto display = (*section)["display_required"].value<bool>()) {
            snapshot.power.displayRequired = *display;
        }
        if (const auto scheme = (*section)["switch_power_scheme"].value<bool>()) {
            snapshot.power.switchPowerScheme = *scheme;
        }
    }

    // [priority]：max_level 上限无 realtime，显式拒绝。
    if (auto* section = table["priority"].as_table()) {
        if (const auto enabled = (*section)["enabled"].value<bool>()) {
            snapshot.priority.enabled = *enabled;
        }
        if (const auto level = (*section)["max_level"].value<std::string>()) {
            auto parsed = ParsePriorityLevel(*level);
            if (parsed) {
                snapshot.priority.maxLevel = parsed.Value();
            }
            // realtime 或非法级别保持默认。
        }
    }

    // [gpu_heartbeat]：R3 Experimental。
    if (auto* section = table["gpu_heartbeat"].as_table()) {
        if (const auto enabled = (*section)["enabled"].value<bool>()) {
            snapshot.gpuHeartbeat.enabled = *enabled;
        }
        if (const auto load = (*section)["max_measured_load_percent"].value<double>()) {
            snapshot.gpuHeartbeat.maxMeasuredLoadPercent = *load;
        }
    }

    // [scheduler] / [disk_cache]：Experimental / Planned，仅 enabled。
    if (auto* section = table["scheduler"].as_table()) {
        if (const auto enabled = (*section)["enabled"].value<bool>()) {
            snapshot.scheduler.enabled = *enabled;
        }
    }
    if (auto* section = table["disk_cache"].as_table()) {
        if (const auto enabled = (*section)["enabled"].value<bool>()) {
            snapshot.diskCache.enabled = *enabled;
        }
    }

    // [[games]]：数组 of table，每项必须有稳定 id。
    if (const auto games = table["games"].as_array()) {
        for (const auto& element : *games) {
            const toml::table* gameTable = element.as_table();
            if (gameTable == nullptr) {
                continue; // 非表元素跳过
            }
            GameConfig game;
            if (const auto id = (*gameTable)["id"].value<std::string>()) {
                game.id = *id;
            }
            if (const auto name = (*gameTable)["display_name"].value<std::string>()) {
                game.displayName = *name;
            }
            if (const auto names = (*gameTable)["process_names"].as_array()) {
                for (const auto& processName : *names) {
                    if (const auto name = processName.value<std::string>()) {
                        game.processNames.push_back(*name);
                    }
                }
            }
            if (const auto title =
                    (*gameTable)["window_title_contains"].value<std::string>()) {
                game.windowTitleContains = *title;
            }
            if (const auto pause =
                    (*gameTable)["pause_when_background"].value<bool>()) {
                game.pauseWhenBackground = *pause;
            }
            // 缺 id 的条目跳过。
            if (!game.id.empty()) {
                snapshot.games.push_back(std::move(game));
            }
        }
    }

    return common::Result<ConfigSnapshot>::Success(std::move(snapshot));
}

} // namespace optimizer::config
