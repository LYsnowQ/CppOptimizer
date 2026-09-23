#include "power/power_scheme.hpp"

#include <windows.h>
#include <objbase.h> // StringFromGUID2 / CLSIDFromString
#include <powrprof.h>

#include <fstream>
#include <string>
#include <utility>

namespace optimizer::power {

namespace {

// GUID -> 归一化文本（大写、带横线、无大括号）。
std::string FormatGuid(const GUID& guid) {
    wchar_t buffer[40] = {};
    // 用字符串形式而不是手工拼字段：只有一处格式化口径。
    const int written = ::StringFromGUID2(guid, buffer, 40);
    if (written <= 0) {
        return {};
    }
    std::string text;
    for (int i = 0; i < written - 1; ++i) { // 末位是 NUL
        const wchar_t ch = buffer[i];
        if (ch == L'{' || ch == L'}') {
            continue;
        }
        const wchar_t upper = (ch >= L'a' && ch <= L'z') ? ch - L'a' + L'A' : ch;
        text.push_back(upper <= 0x7F ? static_cast<char>(upper) : '?');
    }
    return text;
}

// 归一化文本 -> GUID（要求已是 8-4-4-4-12 形式）。
bool ParseGuid(std::string_view text, GUID& out) noexcept {
    std::wstring wide(text.begin(), text.end());
    return ::CLSIDFromString(wide.c_str(), &out) == S_OK;
}

class RealPowerSchemeBackend final : public PowerSchemeBackend {
public:
    common::Result<std::string> GetActive() override {
        GUID* active = nullptr;
        const DWORD status = ::PowerGetActiveScheme(nullptr, &active);
        if (status != ERROR_SUCCESS || active == nullptr) {
            if (active != nullptr) {
                ::LocalFree(active);
            }
            return common::Result<std::string>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(status), "PowerGetActiveScheme"));
        }
        const std::string text = FormatGuid(*active);
        ::LocalFree(active);
        if (text.empty()) {
            return common::Result<std::string>::Failure(common::Error::Unsupported(
                "PowerGetActiveScheme", L"活动电源计划 GUID 无法格式化"));
        }
        return common::Result<std::string>::Success(text);
    }

    common::Result<void> SetActive(std::string_view guid) override {
        GUID parsed{};
        if (!ParseGuid(guid, parsed)) {
            return common::Result<void>::Failure(common::Error::Validation(
                "PowerSetActiveScheme", L"电源计划 GUID 形式非法"));
        }
        const DWORD status = ::PowerSetActiveScheme(nullptr, &parsed);
        if (status != ERROR_SUCCESS) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(status), "PowerSetActiveScheme"));
        }
        return common::Result<void>::Success();
    }
};

} // namespace

std::optional<std::string> NormalizeSchemeGuid(std::string_view text) noexcept {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char ch : text) {
        if (ch == '{' || ch == '}' || ch == ' ' || ch == '\t') {
            continue;
        }
        const char upper =
            (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
        cleaned.push_back(upper);
    }
    if (cleaned.size() != 36) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
        const char ch = cleaned[i];
        const bool separator = (i == 8 || i == 13 || i == 18 || i == 23);
        if (separator) {
            if (ch != '-') {
                return std::nullopt;
            }
            continue;
        }
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
        if (!hex) {
            return std::nullopt;
        }
    }
    return cleaned;
}

std::optional<std::string> ResolveSchemeArgument(std::string_view text) noexcept {
    std::string folded;
    folded.reserve(text.size());
    for (const char ch : text) {
        const char lower =
            (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
        folded.push_back(lower == '_' ? '-' : lower);
    }
    if (folded == "balanced") {
        return std::string(kSchemeBalanced);
    }
    if (folded == "high-performance" || folded == "highperformance") {
        return std::string(kSchemeHighPerformance);
    }
    if (folded == "power-saver" || folded == "powersaver"
        || folded == "saver") {
        return std::string(kSchemePowerSaver);
    }
    return NormalizeSchemeGuid(text); // 也接受直接给出的 GUID 文本
}

PowerSchemeBackend& Win32PowerSchemeBackend() noexcept {
    static RealPowerSchemeBackend backend;
    return backend;
}

common::Result<SchemeSwitchOutcome> ApplyScheme(
    PowerSchemeBackend& backend, std::string_view targetGuid) {
    const auto normalized = NormalizeSchemeGuid(targetGuid);
    if (!normalized.has_value()) {
        return common::Result<SchemeSwitchOutcome>::Failure(
            common::Error::Validation("ApplyScheme", L"目标 GUID 形式非法"));
    }
    // 第一步：先读回原 GUID。读不到就不切——没有回滚信息不得修改全局状态。
    const auto previous = backend.GetActive();
    if (!previous) {
        return common::Result<SchemeSwitchOutcome>::Failure(
            previous.ErrorValue());
    }
    SchemeSwitchOutcome outcome;
    outcome.previousGuid = previous.Value();
    outcome.targetGuid = *normalized;
    const auto applied = backend.SetActive(*normalized);
    if (!applied) {
        return common::Result<SchemeSwitchOutcome>::Failure(
            applied.ErrorValue());
    }
    // 第二步：只读读回校验（切换“调用成功”不等于“状态已生效”）。
    const auto active = backend.GetActive();
    if (!active) {
        return common::Result<SchemeSwitchOutcome>::Failure(
            active.ErrorValue());
    }
    outcome.activeGuid = active.Value();
    outcome.changed = outcome.activeGuid == outcome.targetGuid;
    return common::Result<SchemeSwitchOutcome>::Success(std::move(outcome));
}

common::Result<SchemeRestoreOutcome> RestoreScheme(
    PowerSchemeBackend& backend, std::string_view savedGuid) {
    const auto normalized = NormalizeSchemeGuid(savedGuid);
    if (!normalized.has_value()) {
        return common::Result<SchemeRestoreOutcome>::Failure(
            common::Error::Validation("RestoreScheme", L"保存的 GUID 形式非法"));
    }
    const auto applied = backend.SetActive(*normalized);
    if (!applied) {
        return common::Result<SchemeRestoreOutcome>::Failure(
            applied.ErrorValue());
    }
    const auto active = backend.GetActive();
    if (!active) {
        return common::Result<SchemeRestoreOutcome>::Failure(active.ErrorValue());
    }
    SchemeRestoreOutcome outcome;
    outcome.savedGuid = *normalized;
    outcome.activeGuid = active.Value();
    outcome.restored = outcome.activeGuid == outcome.savedGuid;
    return common::Result<SchemeRestoreOutcome>::Success(std::move(outcome));
}

std::optional<std::string> ParseSavedSchemeContent(
    std::string_view content) noexcept {
    std::size_t lineStart = 0;
    bool envelopeSeen = false;
    std::optional<std::string> saved;
    while (lineStart <= content.size()) {
        const std::size_t lineEnd = content.find('\n', lineStart);
        std::string_view line = content.substr(
            lineStart, (lineEnd == std::string_view::npos ? content.size() : lineEnd) -
                           lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!envelopeSeen) {
            if (line != kSchemeSaveEnvelope) {
                return std::nullopt; // 首行必须为信封，否则整份不接受
            }
            envelopeSeen = true;
        } else if (line.substr(0, 6) == "saved=") {
            const auto normalized = NormalizeSchemeGuid(line.substr(6));
            if (!normalized.has_value()) {
                return std::nullopt; // 字段在但 GUID 非法 -> 不接受（不静默当“没有”）
            }
            saved = normalized;
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    if (!envelopeSeen) {
        return std::nullopt;
    }
    return saved;
}

common::Result<void> SaveSchemeGuid(const std::filesystem::path& path,
                                    std::string_view guid) noexcept {
    if (path.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "SaveSchemeGuid", L"路径不能为空"));
    }
    const auto normalized = NormalizeSchemeGuid(guid);
    if (!normalized.has_value()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "SaveSchemeGuid", L"GUID 形式非法"));
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(ec.value()),
                "create_directories(power scheme save)"));
        }
    }
    std::filesystem::path tempPath = path;
    tempPath += L".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(::GetLastError()),
                "open power scheme save temp file"));
        }
        out << kSchemeSaveEnvelope << '\n' << "saved=" << *normalized << '\n';
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tempPath, ec);
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(::GetLastError()),
                "flush power scheme save temp file"));
        }
        out.close();
    }
    if (!::MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = static_cast<std::uint32_t>(::GetLastError());
        std::filesystem::remove(tempPath, ec);
        return common::Result<void>::Failure(
            common::Error::FromWin32(code, "MoveFileExW(power scheme save)"));
    }
    return common::Result<void>::Success();
}

common::Result<std::optional<std::string>> ReadSavedSchemeGuid(
    const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return common::Result<std::optional<std::string>>::Failure(
            common::Error::Validation("ReadSavedSchemeGuid", L"路径不能为空"));
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return common::Result<std::optional<std::string>>::Failure(
            common::Error::Validation("ReadSavedSchemeGuid",
                                     L"保存路径是目录，不是文件"));
    }
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            return common::Result<std::optional<std::string>>::Failure(
                common::Error::FromWin32(static_cast<std::uint32_t>(ec.value()),
                                         "exists(power scheme save)"));
        }
        return common::Result<std::optional<std::string>>::Success(
            std::nullopt); // 尚无恢复依据（不是错误）
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return common::Result<std::optional<std::string>>::Failure(
            common::Error::FromWin32(
                static_cast<std::uint32_t>(::GetLastError()),
                "open power scheme save"));
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return common::Result<std::optional<std::string>>::Success(
        ParseSavedSchemeContent(content));
}

} // namespace optimizer::power
