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
#include <limits>
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

// 未知键核对（如实上报，不改变解析结果）：known 为该表允许的键集合，
// prefix 为点分路径前缀（顶层为空）。
void CollectUnknownKeys(const toml::table& table,
                        std::initializer_list<std::string_view> known,
                        std::string_view prefix,
                        std::vector<std::string>& out) {
    for (const auto& [key, node] : table) {
        (void)node;
        const std::string name(key.str());
        bool isKnown = false;
        for (const auto candidate : known) {
            if (name == candidate) {
                isKnown = true;
                break;
            }
        }
        if (!isKnown) {
            out.push_back(prefix.empty() ? name
                                         : std::string(prefix) + "." + name);
        }
    }
}

} // namespace

common::Result<ConfigVersion> ParseConfigVersion(std::string_view text) {
    // 规范形式：MAJOR.MINOR.PATCH[-PRERELEASE]（ASCII；段位数 <=4；预发布 <=32 字符）。
    constexpr std::size_t kMaxTextLength = 48;
    constexpr std::size_t kMaxDigitsPerSegment = 4;
    constexpr std::size_t kMaxPrereleaseLength = 32;
    if (text.empty() || text.size() > kMaxTextLength) {
        return common::Result<ConfigVersion>::Failure(common::Error::Validation(
            "ParseConfigVersion", L"version must be 1..48 ASCII characters"));
    }
    const auto dash = text.find('-');
    const std::string_view body =
        dash == std::string_view::npos ? text : text.substr(0, dash);
    const std::string_view prerelease =
        dash == std::string_view::npos ? std::string_view()
                                       : text.substr(dash + 1);
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    std::uint32_t segments[3] = {0, 0, 0};
    std::size_t segmentIndex = 0;
    std::size_t digits = 0;
    bool segmentFirstDigitZero = false; // 用于仅拒绝“多位数且以 0 开头”的段（0 本身合法）
    for (const char c : body) {
        if (c == '.') {
            if (digits == 0 || segmentIndex >= 2) {
                return common::Result<ConfigVersion>::Failure(
                    common::Error::Validation(
                        "ParseConfigVersion",
                        L"version must be MAJOR.MINOR.PATCH (three numeric segments)"));
            }
            ++segmentIndex;
            digits = 0;
            segmentFirstDigitZero = false;
            continue;
        }
        if (!isDigit(c)) {
            return common::Result<ConfigVersion>::Failure(
                common::Error::Validation(
                    "ParseConfigVersion",
                    L"version must be MAJOR.MINOR.PATCH (ASCII digits and '.')"));
        }
        if (digits >= kMaxDigitsPerSegment) {
            return common::Result<ConfigVersion>::Failure(
                common::Error::Validation(
                    "ParseConfigVersion", L"version segments must be <= 4 digits"));
        }
        if (digits == 0) {
            segmentFirstDigitZero = (c == '0');
        } else if (segmentFirstDigitZero) {
            // 仅拒绝“多位数且以 0 开头”（如 01）；单独的 0 合法（如 1.0.0）。
            return common::Result<ConfigVersion>::Failure(
                common::Error::Validation(
                    "ParseConfigVersion", L"version segments must not have leading zeros"));
        }
        segments[segmentIndex] =
            segments[segmentIndex] * 10 + static_cast<std::uint32_t>(c - '0');
        ++digits;
    }
    if (digits == 0 || segmentIndex != 2) {
        return common::Result<ConfigVersion>::Failure(common::Error::Validation(
            "ParseConfigVersion",
            L"version must be MAJOR.MINOR.PATCH (three numeric segments)"));
    }
    if (dash != std::string_view::npos) {
        if (prerelease.empty() || prerelease.size() > kMaxPrereleaseLength) {
            return common::Result<ConfigVersion>::Failure(
                common::Error::Validation(
                    "ParseConfigVersion",
                    L"prerelease must be 1..32 characters after '-'"));
        }
        if (prerelease.front() == '.' || prerelease.back() == '.' ||
            prerelease.find("..") != std::string_view::npos) {
            return common::Result<ConfigVersion>::Failure(
                common::Error::Validation(
                    "ParseConfigVersion",
                    L"prerelease must not contain empty segments"));
        }
        for (const char c : prerelease) {
            const bool allowed = isDigit(c) || (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
            if (!allowed) {
                return common::Result<ConfigVersion>::Failure(
                    common::Error::Validation(
                        "ParseConfigVersion",
                        L"prerelease accepts ASCII letters/digits/'.'/'-' only"));
            }
        }
    }
    ConfigVersion version;
    version.major = segments[0];
    version.minor = segments[1];
    version.patch = segments[2];
    version.prerelease = std::string(prerelease);
    return common::Result<ConfigVersion>::Success(std::move(version));
}

std::string FormatConfigVersion(const ConfigVersion& version) {
    std::string text = std::to_string(version.major) + "." +
                       std::to_string(version.minor) + "." +
                       std::to_string(version.patch);
    if (!version.prerelease.empty()) {
        text += "-" + version.prerelease; // 预发布标识（beta 等）原样显示
    }
    return text;
}

bool AllowsLocalReversibleActions(RunMode mode,
                                  const LayerConfig& layers) noexcept {
    // 叠加门禁：模式（非 observe）与维护层（或应急层）都要允许。
    const bool modeAllows = mode != RunMode::Observe;
    const bool layerAllows = layers.maintenance || layers.emergency;
    return modeAllows && layerAllows;
}

bool AllowsSystemLevelActions(RunMode mode,
                              const LayerConfig& layers) noexcept {
    // R2/R3 仅在 experimental + 应急层开启时进入“门禁评估”（真实动作仍须逐条满足全部安全条件）。
    return mode == RunMode::Experimental && layers.emergency;
}

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

    if (table.contains("version")) {
        // CFG-008：三段数字 + 可选预发布标识（如 "1.0.0" / "1.2.0-beta.1"）；
        // 旧式整数（`version = 1`）按兼容处理为 "1.0.0"（既有配置零回归）；
        // 其它主版本、非法形式、非法字符/超长均拒绝（不做宽松转换）。
        const auto* versionNode = table.get("version");
        std::string versionText;
        // 先判类型再取值：toml++ 的 value<T>() 允许宽松转换（布尔会被当成整数），
        // 而本字段要求“类型不符必须拒绝”（不做宽松转换）。
        if (versionNode->type() == toml::node_type::integer) {
            versionText = std::to_string(*versionNode->value<std::int64_t>()) +
                          ".0.0"; // 旧式整数兼容
        } else if (versionNode->type() == toml::node_type::string) {
            versionText = *versionNode->value<std::string>();
        } else {
            return common::Result<ConfigSnapshot>::Failure(
                common::Error::Validation(
                    "LoadConfig",
                    L"version must be a string \"M.m.p[-pre]\" (or legacy integer)"));
        }
        auto parsedVersion = ParseConfigVersion(versionText);
        if (!parsedVersion) {
            return common::Result<ConfigSnapshot>::Failure(
                parsedVersion.ErrorValue());
        }
        if (parsedVersion.Value().major != kSupportedConfigMajor) {
            return common::Result<ConfigSnapshot>::Failure(
                common::Error::Validation(
                    "LoadConfig",
                    L"unsupported config version major (supported: 1.x.y)"));
        }
        snapshot.version = std::move(parsedVersion.Value());
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

    // [policy]：策略阈值。数值须落在 int32 范围；节缺失时用默认值（恒合法）。
    if (auto* section = table["policy"].as_table()) {
        if (const auto v =
                (*section)["comfortable_margin_percent"].value<std::int64_t>()) {
            if (*v >= std::numeric_limits<std::int32_t>::min() &&
                *v <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.policy.comfortableMarginPercent =
                    static_cast<std::int32_t>(*v);
            }
        }
        if (const auto v =
                (*section)["adequate_margin_percent"].value<std::int64_t>()) {
            if (*v >= std::numeric_limits<std::int32_t>::min() &&
                *v <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.policy.adequateMarginPercent =
                    static_cast<std::int32_t>(*v);
            }
        }
        if (const auto v =
                (*section)["tight_margin_percent"].value<std::int64_t>()) {
            if (*v >= std::numeric_limits<std::int32_t>::min() &&
                *v <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.policy.tightMarginPercent =
                    static_cast<std::int32_t>(*v);
            }
        }
        if (const auto v = (*section)["cooldown_ms"].value<std::int64_t>()) {
            if (*v >= std::numeric_limits<std::int32_t>::min() &&
                *v <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.policy.cooldownMs = static_cast<std::int32_t>(*v);
            }
        }
        // user_away_idle_seconds：0 = 关闭用户在场门禁（ACT-004）；1..86400 秒内无输入
        // 视用户不在场。越界（负数/超 86400）属语义错误在下方统一拒绝。
        if (const auto v =
                (*section)["user_away_idle_seconds"].value<std::int64_t>()) {
            if (*v >= std::numeric_limits<std::int32_t>::min() &&
                *v <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.policy.userAwayIdleSeconds =
                    static_cast<std::int32_t>(*v);
            }
        }
        // halt_after_action_failures：0 = 关闭 R1 动作连续失败停摆（IPC-017）；
        // 1..10000 次连续 R1 动作失败后执行器停摆。越界属语义错误在下方统一拒绝。
        if (const auto v =
                (*section)["halt_after_action_failures"].value<std::int64_t>()) {
            if (*v >= std::numeric_limits<std::int32_t>::min() &&
                *v <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.policy.haltAfterActionFailures =
                    static_cast<std::int32_t>(*v);
            }
        }
    }
    // 阈值有序性与范围校验：无论节是否存在，默认值恒合法；
    // 违序/越界配置是语义错误，直接拒绝（错误阈值会产生错误决策）。
    if (!(snapshot.policy.tightMarginPercent >= 0 &&
          snapshot.policy.tightMarginPercent <
              snapshot.policy.adequateMarginPercent &&
          snapshot.policy.adequateMarginPercent <
              snapshot.policy.comfortableMarginPercent &&
          snapshot.policy.comfortableMarginPercent <= 100) ||
        snapshot.policy.cooldownMs < 0 ||
        snapshot.policy.userAwayIdleSeconds < 0 ||
        snapshot.policy.userAwayIdleSeconds > 86400 ||
        snapshot.policy.haltAfterActionFailures < 0 ||
        snapshot.policy.haltAfterActionFailures > 10000) {
        return common::Result<ConfigSnapshot>::Failure(
            common::Error::Validation(
                "LoadConfig",
                L"[policy] thresholds must satisfy "
                L"0 <= tight < adequate < comfortable <= 100, cooldown_ms >= 0, "
                L"user_away_idle_seconds in 0..86400 (0 = gate off), "
                L"halt_after_action_failures in 0..10000 (0 = off)"));
    }

    // [ipc]：Safe Mode 门禁窗口参数（Agent 受理侧；缺省与 SafeModeGuard::Options 一致）。
    // 数值仅在 int32 范围内写入，随后做 1..上限 的语义校验；越界/为零是语义错误
    // （0 时长使门禁无实际暂停，等同失效），直接拒绝而非静默回退默认。
    if (auto* section = table["ipc"].as_table()) {
        if (const auto enabled = (*section)["safe_mode_enabled"].value<bool>()) {
            snapshot.ipc.safeMode.enabled = *enabled;
        }
        if (const auto failures =
                (*section)["safe_mode_failures"].value<std::int64_t>()) {
            if (*failures >= std::numeric_limits<std::int32_t>::min() &&
                *failures <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.ipc.safeMode.failuresToEnter =
                    static_cast<std::int32_t>(*failures);
            }
        }
        if (const auto window =
                (*section)["safe_mode_window_ms"].value<std::int64_t>()) {
            if (*window >= std::numeric_limits<std::int32_t>::min() &&
                *window <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.ipc.safeMode.countingWindowMs =
                    static_cast<std::int32_t>(*window);
            }
        }
        if (const auto cooldown =
                (*section)["safe_mode_cooldown_ms"].value<std::int64_t>()) {
            if (*cooldown >= std::numeric_limits<std::int32_t>::min() &&
                *cooldown <= std::numeric_limits<std::int32_t>::max()) {
                snapshot.ipc.safeMode.cooldownMs =
                    static_cast<std::int32_t>(*cooldown);
            }
        }
    }
    if (!(snapshot.ipc.safeMode.failuresToEnter >= 1 &&
          snapshot.ipc.safeMode.failuresToEnter <= 100 &&
          snapshot.ipc.safeMode.countingWindowMs >= 1 &&
          snapshot.ipc.safeMode.countingWindowMs <= 600000 &&
          snapshot.ipc.safeMode.cooldownMs >= 1 &&
          snapshot.ipc.safeMode.cooldownMs <= 600000)) {
        return common::Result<ConfigSnapshot>::Failure(
            common::Error::Validation(
                "LoadConfig",
                L"[ipc] safe_mode must satisfy "
                L"1 <= failures <= 100, 1 <= window_ms <= 600000, "
                L"1 <= cooldown_ms <= 600000"));
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

    // 未知键核对（如实上报，不改变解析结果）：与“缺失的键使用默认值”原则共存——
    // 未知键仍被忽略以保证兼容，但用户必须能看到（否则拼写错误被静默吞掉）。
    CollectUnknownKeys(
        table,
        {"version", "application", "logging", "layers", "power", "priority",
         "memory", "gpu_heartbeat", "scheduler", "disk_cache", "policy",
         "ipc", "games", "agent"},
        "", snapshot.unknownKeys);
    const auto collectSection =
        [&snapshot, &table](const char* section,
                            std::initializer_list<std::string_view> keys) {
            if (const auto* sectionTable = table[section].as_table()) {
                CollectUnknownKeys(*sectionTable, keys, section,
                                   snapshot.unknownKeys);
            }
        };
    collectSection("application", {"mode", "safe_mode_on_recovery_error"});
    collectSection("logging", {"level", "directory", "max_file_mb", "max_files",
                               "console"});
    collectSection("layers", {"monitoring", "maintenance", "emergency"});
    collectSection("power", {"execution_required", "display_required",
                             "switch_power_scheme"});
    collectSection("priority", {"enabled", "max_level"});
    collectSection("memory", {"query_enabled", "scheduled_clean_enabled",
                              "allow_native_write", "max_clean_level"});
    collectSection("gpu_heartbeat", {"enabled", "max_measured_load_percent"});
    collectSection("scheduler", {"enabled"});
    collectSection("disk_cache", {"enabled"});
    collectSection("policy", {"comfortable_margin_percent",
                               "adequate_margin_percent", "tight_margin_percent",
                               "cooldown_ms", "user_away_idle_seconds",
                               "halt_after_action_failures"});
    collectSection("ipc", {"safe_mode_enabled", "safe_mode_failures",
                            "safe_mode_window_ms", "safe_mode_cooldown_ms"});
    collectSection("agent", {"form"});
    if (const auto* games = table["games"].as_array()) {
        std::size_t index = 0;
        for (const auto& entry : *games) {
            if (const auto* gameTable = entry.as_table()) {
                CollectUnknownKeys(*gameTable,
                                   {"id", "display_name", "process_names",
                                    "pause_when_background",
                                    "window_title_contains"},
                                   "games[" + std::to_string(index) + "]",
                                   snapshot.unknownKeys);
            }
            ++index;
        }
    }

    // [agent]：常驻 Agent 形态（**声明的意图**，不触发任何注册动作）。
    // 受理“启动项 + 托盘”（默认）/“计划任务”/“SCM 服务”；其余取值显式拒绝。
    // 关键约束：配置只表达选择，**不自动注册、不自动提权**——注册始终是显式命令动作。
    if (auto* agentSection = table["agent"].as_table()) {
        if (const auto form = (*agentSection)["form"].value<std::string>()) {
            const std::string lower = ToLower(*form);
            if (lower != "startup_tray" && lower != "task" && lower != "service") {
                return common::Result<ConfigSnapshot>::Failure(
                    common::Error::Validation(
                        "LoadConfig",
                        L"agent.form: 仅支持 \"startup_tray\"（默认）/\"task\"/\"service\""
                        L"（未知形态不做宽松猜测）"));
            }
            snapshot.agent.form = lower;
        }
    }

    return common::Result<ConfigSnapshot>::Success(std::move(snapshot));
}

} // namespace optimizer::config
