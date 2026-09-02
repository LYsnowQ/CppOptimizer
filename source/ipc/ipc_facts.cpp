#include "ipc/ipc_facts.hpp"

#include <optional>

namespace optimizer::ipc {

namespace {

// 键字符集：ASCII 字母/数字/下划线/连字符。
bool IsAsciiKeyChar(char c) noexcept {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9');
    return alnum || c == '_' || c == '-';
}

// 控制字节（0x00..0x1F、0x7F）一律拒绝：键/值/回显都不允许被终端或日志注入。
bool HasControlByte(std::string_view text) noexcept {
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7F) {
            return true;
        }
    }
    return false;
}

std::vector<std::byte> BytesFromText(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char ch : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return bytes;
}

// 校验键；失败返回错误消息。
std::optional<std::wstring> ValidateKey(std::string_view key) {
    if (key.empty()) {
        return std::wstring(L"键为空");
    }
    if (key.size() > kMaxFactsKeyBytes) {
        return std::wstring(L"键超过长度上限");
    }
    if (HasControlByte(key)) {
        return std::wstring(L"键含控制字符");
    }
    for (const char ch : key) {
        if (!IsAsciiKeyChar(ch)) {
            return std::wstring(L"键含非法字符（仅 ASCII 字母/数字/_/-）");
        }
    }
    return std::nullopt;
}

// 校验值；失败返回错误消息。
std::optional<std::wstring> ValidateValue(std::string_view value) {
    if (value.size() > kMaxFactsValueBytes) {
        return std::wstring(L"值超过长度上限");
    }
    if (HasControlByte(value)) {
        return std::wstring(L"值含控制字符");
    }
    return std::nullopt;
}

// 校验整份事实列表（条数/键/值/重复），失败返回错误消息。
std::optional<std::wstring> ValidateFacts(std::span<const IpcFact> facts) {
    if (facts.size() > kMaxFactsEntries) {
        return std::wstring(L"事实条数超过上限");
    }
    for (std::size_t i = 0; i < facts.size(); ++i) {
        if (auto error = ValidateKey(facts[i].key)) {
            return error;
        }
        if (auto error = ValidateValue(facts[i].value)) {
            return error;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (facts[j].key == facts[i].key) {
                return std::wstring(L"重复键 ") +
                       std::wstring(facts[i].key.begin(), facts[i].key.end());
            }
        }
    }
    return std::nullopt;
}

common::Error FactsValidationError(std::wstring message) {
    return common::Error::Validation("FactsV1", std::move(message));
}

} // namespace

common::Result<std::vector<IpcFact>> ParseFactsV1(
    std::span<const std::byte> payload) {
    if (payload.empty()) {
        return common::Result<std::vector<IpcFact>>::Failure(
            FactsValidationError(L"空载荷"));
    }

    // 切行：仅 LF 为分隔符；CR 出现即整体拒绝（不接受 CRLF/孤立 CR）。
    // 切出的每个段都要求非空：空行、尾随换行产生的空尾段一律拒绝。
    std::vector<std::string> lines;
    std::string current;
    for (const std::byte b : payload) {
        const char ch = static_cast<char>(b);
        if (ch == '\r') {
            return common::Result<std::vector<IpcFact>>::Failure(
                FactsValidationError(L"载荷含 CR（行尾只允许 LF）"));
        }
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(std::move(current));

    for (const std::string& line : lines) {
        if (line.empty()) {
            return common::Result<std::vector<IpcFact>>::Failure(
                FactsValidationError(L"载荷含空行或尾随换行"));
        }
    }

    if (lines.front() != kFactsEnvelopeV1) {
        return common::Result<std::vector<IpcFact>>::Failure(
            FactsValidationError(L"首行信封必须为 CPOPFACTS/1"));
    }

    std::vector<IpcFact> facts;
    facts.reserve(lines.size() - 1);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (facts.size() >= kMaxFactsEntries) {
            return common::Result<std::vector<IpcFact>>::Failure(
                FactsValidationError(L"事实条数超过上限"));
        }
        const std::string& line = lines[i];
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            return common::Result<std::vector<IpcFact>>::Failure(
                FactsValidationError(L"事实行缺少 '='"));
        }
        IpcFact fact;
        fact.key = line.substr(0, equals);
        fact.value = line.substr(equals + 1);

        if (auto error = ValidateKey(fact.key)) {
            return common::Result<std::vector<IpcFact>>::Failure(
                FactsValidationError(*error));
        }
        if (auto error = ValidateValue(fact.value)) {
            return common::Result<std::vector<IpcFact>>::Failure(
                FactsValidationError(*error));
        }
        for (const IpcFact& existing : facts) {
            if (existing.key == fact.key) {
                return common::Result<std::vector<IpcFact>>::Failure(
                    FactsValidationError(L"重复键"));
            }
        }
        facts.push_back(std::move(fact));
    }
    return common::Result<std::vector<IpcFact>>::Success(std::move(facts));
}

common::Result<std::vector<std::byte>> SerializeFactsV1(
    std::span<const IpcFact> facts) {
    if (auto error = ValidateFacts(facts)) {
        return common::Result<std::vector<std::byte>>::Failure(
            FactsValidationError(*error));
    }
    std::string text(kFactsEnvelopeV1);
    for (const IpcFact& fact : facts) {
        text.push_back('\n');
        text.append(fact.key);
        text.push_back('=');
        text.append(fact.value);
    }
    return common::Result<std::vector<std::byte>>::Success(
        BytesFromText(text));
}

std::string FormatFactsSummary(std::span<const IpcFact> facts) {
    std::string out = "facts ok (";
    out += std::to_string(facts.size());
    out += ')';
    if (facts.empty()) {
        return out;
    }
    out += ':';
    // 逐条回显；无法容纳下一条（预留 " …" 3 字节）即截断并标记省略。
    for (std::size_t i = 0; i < facts.size(); ++i) {
        std::string entry;
        entry.reserve(1 + facts[i].key.size() + 1 + facts[i].value.size());
        entry.push_back(' ');
        entry += facts[i].key;
        entry.push_back('=');
        entry += facts[i].value;
        if (out.size() + entry.size() + 3 > kFactsSummaryMaxBytes) {
            out += " …";
            return out;
        }
        out += entry;
        if (i + 1 == facts.size()) {
            return out;
        }
    }
    return out; // 防御：不可达（facts 非空时末尾已返回）。
}

} // namespace optimizer::ipc
