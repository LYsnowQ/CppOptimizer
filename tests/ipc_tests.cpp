#include "ipc/ipc_protocol.hpp"
#include "ipc/ipc_session.hpp"
#include "ipc/ipc_transport.hpp"
#include "ipc/ipc_facts.hpp"
#include "ipc/ipc_credentials.hpp"

#include "common/unique_resource.hpp"

#include <windows.h>
#include <sddl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::ErrorDomain;
using optimizer::common::Result;
using optimizer::ipc::IpcClientBackend;
using optimizer::ipc::IpcClientIdentity;
using optimizer::ipc::IpcErrorCode;
using optimizer::ipc::IpcFact;
using optimizer::ipc::IpcHeader;
using optimizer::ipc::IpcMessageType;
using optimizer::ipc::IpcReply;
using optimizer::ipc::IpcRequest;
using optimizer::ipc::IpcRoundTrip;
using optimizer::ipc::IpcRoundTripSession;
using optimizer::ipc::IpcServeResult;
using optimizer::ipc::IpcServerBackend;
using optimizer::ipc::IpcSession;
using optimizer::ipc::IpcSessionEndReason;
using optimizer::ipc::IpcClientVerdict;
using optimizer::ipc::IpcFrameRequest;
using optimizer::ipc::IpcPeriodicReportOptions;
using optimizer::ipc::IpcReportGapAfterFailures;
using optimizer::ipc::RunPeriodicReporter;
using optimizer::ipc::RunConcurrentServer;
using optimizer::ipc::kIpcHeaderSize;
using optimizer::ipc::kIpcMagic;
using optimizer::ipc::kIpcVersion;
using optimizer::ipc::kMaxPayloadLength;
using optimizer::ipc::MakeIpcHeader;
using optimizer::ipc::MessageTypeToString;
using optimizer::ipc::ParseHeader;
using optimizer::ipc::ParseMessageType;
using optimizer::ipc::SerializeHeader;
using optimizer::ipc::SerializeFactsV1;
using optimizer::ipc::ParseFactsV1;
using optimizer::ipc::ValidateFactsV1Schema;
using optimizer::ipc::FormatFactsSummary;
using optimizer::ipc::kFactsSummaryMaxBytes;
using optimizer::ipc::kMaxFactsEntries;
using optimizer::ipc::kMaxFactsKeyBytes;
using optimizer::ipc::kMaxFactsValueBytes;
using optimizer::ipc::ValidateIpcHeader;

// ---------- fake 服务端后端（记录调用序列，可注入帧与失败） ----------

class FakeIpcServerBackend final : public IpcServerBackend {
public:
    bool failCreate = false;
    bool failAccept = false;
    std::uint32_t acceptErrorCode = ERROR_TIMEOUT;
    bool failRead = false;
    bool failWrite = false;
    bool failDisconnect = false;
    std::uint32_t failErrorCode = ERROR_BROKEN_PIPE;

    std::uint32_t clientPid = 1234;
    std::uint32_t clientSessionId = 2;
    std::wstring clientSid; // 默认空 = 未知（未启用 SID 授权时不影响既有测试）

    std::vector<std::byte> incoming; // 模拟客户端注入的完整帧
    std::size_t readOffset = 0;
    std::vector<std::byte> outgoing; // 记录服务端写出的帧
    int createCount = 0;
    int acceptCount = 0;
    int disconnectCount = 0;
    int closeCount = 0;
    // 按调用次数的读失败注入（多帧/会话超时测试）：0 = 不启用。
    std::size_t readCallCount = 0;
    std::size_t failReadAtCall = 0;      // 从第 N 次 ReadAll 起强制失败
    std::uint32_t failReadAtErrorCode = ERROR_TIMEOUT;

    Result<void> CreateAndListen() override {
        ++createCount;
        if (failCreate) {
            return Result<void>::Failure(Error::FromWin32(
                ERROR_ACCESS_DENIED, "CreateAndListen"));
        }
        return Result<void>::Success();
    }

    Result<void> AcceptClient(std::chrono::milliseconds) override {
        ++acceptCount;
        if (failAccept) {
            return Result<void>::Failure(
                Error::FromWin32(acceptErrorCode, "AcceptClient"));
        }
        return Result<void>::Success();
    }

    Result<void> ReadAll(std::span<std::byte> buffer,
                         std::chrono::milliseconds) override {
        ++readCallCount;
        if (failReadAtCall != 0 && readCallCount >= failReadAtCall) {
            return Result<void>::Failure(
                Error::FromWin32(failReadAtErrorCode, "ReadAll"));
        }
        if (failRead) {
            return Result<void>::Failure(
                Error::FromWin32(failErrorCode, "ReadAll"));
        }
        if (buffer.size() > incoming.size() - readOffset) {
            return Result<void>::Failure(
                Error::FromWin32(ERROR_BROKEN_PIPE, "ReadAll"));
        }
        std::copy(incoming.begin() + static_cast<std::ptrdiff_t>(readOffset),
                  incoming.begin() +
                      static_cast<std::ptrdiff_t>(readOffset + buffer.size()),
                  buffer.begin());
        readOffset += buffer.size();
        return Result<void>::Success();
    }

    Result<void> WriteAll(std::span<const std::byte> buffer,
                          std::chrono::milliseconds) override {
        if (failWrite) {
            return Result<void>::Failure(
                Error::FromWin32(failErrorCode, "WriteAll"));
        }
        outgoing.insert(outgoing.end(), buffer.begin(), buffer.end());
        return Result<void>::Success();
    }

    Result<void> DisconnectClient() override {
        ++disconnectCount;
        if (failDisconnect) {
            return Result<void>::Failure(
                Error::FromWin32(failErrorCode, "DisconnectClient"));
        }
        return Result<void>::Success();
    }

    void Close() noexcept override {
        ++closeCount;
    }

    std::uint32_t ClientPid() const noexcept override {
        return clientPid;
    }

    std::uint32_t ClientSessionId() const noexcept override {
        return clientSessionId;
    }

    std::wstring ClientUserSid() const override {
        return clientSid;
    }
};

// ---------- fake 客户端后端 ----------

class FakeIpcClientBackend final : public IpcClientBackend {
public:
    bool failConnect = false;
    bool failWrite = false;
    bool failRead = false;
    std::uint32_t failCode = ERROR_ACCESS_DENIED;
    std::size_t connectCount = 0;          // 已调用 Connect 的次数（含失败尝试）
    std::size_t failConnectCountdown = 0;  // 前 N 次 Connect 强制失败（随后恢复）

    std::wstring connectedPath;
    std::vector<std::byte> incoming; // 应答帧
    std::size_t readOffset = 0;
    std::vector<std::byte> outgoing; // 请求帧
    bool closed = false;

    Result<void> Connect(std::wstring_view pipePath,
                         std::chrono::milliseconds) override {
        ++connectCount;
        if (failConnectCountdown != 0) {
            --failConnectCountdown;
            return Result<void>::Failure(
                Error::FromWin32(failCode, "Connect"));
        }
        if (failConnect) {
            return Result<void>::Failure(Error::FromWin32(failCode, "Connect"));
        }
        connectedPath = std::wstring(pipePath);
        return Result<void>::Success();
    }

    Result<void> WriteAll(std::span<const std::byte> buffer,
                          std::chrono::milliseconds) override {
        if (failWrite) {
            return Result<void>::Failure(Error::FromWin32(failCode, "WriteAll"));
        }
        outgoing.insert(outgoing.end(), buffer.begin(), buffer.end());
        return Result<void>::Success();
    }

    Result<void> ReadAll(std::span<std::byte> buffer,
                         std::chrono::milliseconds) override {
        if (failRead) {
            return Result<void>::Failure(Error::FromWin32(failCode, "ReadAll"));
        }
        if (buffer.size() > incoming.size() - readOffset) {
            return Result<void>::Failure(
                Error::FromWin32(ERROR_BROKEN_PIPE, "ReadAll"));
        }
        std::copy(incoming.begin() + static_cast<std::ptrdiff_t>(readOffset),
                  incoming.begin() +
                      static_cast<std::ptrdiff_t>(readOffset + buffer.size()),
                  buffer.begin());
        readOffset += buffer.size();
        return Result<void>::Success();
    }

    void Close() noexcept override {
        closed = true;
    }
};

// ---------- 帧组装辅助（测试内与实现等价的纯组装，独立于被测代码） ----------

std::vector<std::byte> BuildFrame(IpcMessageType type, std::uint32_t requestId,
                                  std::span<const std::byte> payload) {
    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    SerializeHeader(
        MakeIpcHeader(type, static_cast<std::uint32_t>(payload.size()),
                      requestId),
        headerBytes);
    std::vector<std::byte> frame;
    frame.reserve(kIpcHeaderSize + payload.size());
    frame.insert(frame.end(), headerBytes.begin(), headerBytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::byte> BytesFromText(const std::string& text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char ch : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return bytes;
}

// 解析服务端写出的应答帧（帧头 + 载荷）。
bool ParseWrittenFrame(const std::vector<std::byte>& written,
                       IpcHeader& header,
                       std::vector<std::byte>& payload) {
    if (written.size() < kIpcHeaderSize) {
        return false;
    }
    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    std::copy_n(written.begin(), kIpcHeaderSize, headerBytes.begin());
    auto parsed = ParseHeader(headerBytes);
    if (!parsed) {
        return false;
    }
    header = parsed.Value();
    payload.assign(written.begin() + static_cast<std::ptrdiff_t>(kIpcHeaderSize),
                   written.end());
    return payload.size() == header.payloadLength;
}

// 解析服务端写出的多个应答帧（帧头 + 载荷），保持写入顺序。
bool ParseWrittenFrames(const std::vector<std::byte>& written,
                        std::vector<IpcHeader>& headers,
                        std::vector<std::vector<std::byte>>& payloads) {
    std::size_t offset = 0;
    while (offset < written.size()) {
        if (written.size() - offset < kIpcHeaderSize) {
            return false;
        }
        std::array<std::byte, kIpcHeaderSize> headerBytes{};
        std::copy_n(written.begin() + static_cast<std::ptrdiff_t>(offset),
                    kIpcHeaderSize, headerBytes.begin());
        auto parsed = ParseHeader(headerBytes);
        if (!parsed) {
            return false;
        }
        const std::size_t total =
            kIpcHeaderSize + parsed.Value().payloadLength;
        if (total > written.size() - offset) {
            return false;
        }
        headers.push_back(parsed.Value());
        payloads.push_back(std::vector<std::byte>(
            written.begin() + static_cast<std::ptrdiff_t>(offset +
                                                          kIpcHeaderSize),
            written.begin() +
                static_cast<std::ptrdiff_t>(offset + total)));
        offset += total;
    }
    return true;
}

// ---------- 协议纯函数 ----------

bool TestMakeHeaderFields() {
    const auto header = MakeIpcHeader(IpcMessageType::FactsSnapshot, 100, 42);
    return header.magic == kIpcMagic && header.version == kIpcVersion &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::FactsSnapshot) &&
           header.reserved == 0 && header.payloadLength == 100 &&
           header.requestId == 42;
}

bool TestMessageTypeNames() {
    return std::wstring(MessageTypeToString(IpcMessageType::Ping)) == L"Ping" &&
           std::wstring(MessageTypeToString(IpcMessageType::Ack)) == L"Ack" &&
           std::wstring(MessageTypeToString(IpcMessageType::FactsSnapshot)) ==
               L"FactsSnapshot" &&
           std::wstring(MessageTypeToString(IpcMessageType::Error)) == L"Error";
}

bool TestParseMessageTypeStrict() {
    return ParseMessageType(0x01) == IpcMessageType::Ping &&
           ParseMessageType(0x02) == IpcMessageType::Ack &&
           ParseMessageType(0x03) == IpcMessageType::FactsSnapshot &&
           ParseMessageType(0x04) == IpcMessageType::Error &&
           !ParseMessageType(0x00) && !ParseMessageType(0x05) &&
           !ParseMessageType(0xFF);
}

bool TestValidateHeaderAcceptsValid() {
    return static_cast<bool>(ValidateIpcHeader(MakeIpcHeader(
        IpcMessageType::Ping, kMaxPayloadLength, 1)));
}

bool TestValidateHeaderRejectsBadMagic() {
    auto header = MakeIpcHeader(IpcMessageType::Ping, 0, 1);
    header.magic = 0xDEADBEEF;
    const auto result = ValidateIpcHeader(header);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestValidateHeaderRejectsBadVersion() {
    auto header = MakeIpcHeader(IpcMessageType::Ping, 0, 1);
    header.version = kIpcVersion + 1;
    const auto result = ValidateIpcHeader(header);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestValidateHeaderRejectsUnknownType() {
    auto header = MakeIpcHeader(IpcMessageType::Ping, 0, 1);
    header.type = 0x42;
    const auto result = ValidateIpcHeader(header);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestValidateHeaderRejectsNonzeroReserved() {
    auto header = MakeIpcHeader(IpcMessageType::Ping, 0, 1);
    header.reserved = 1;
    const auto result = ValidateIpcHeader(header);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestValidateHeaderRejectsOversizedPayload() {
    auto header = MakeIpcHeader(IpcMessageType::Ping, kMaxPayloadLength + 1, 1);
    const auto result = ValidateIpcHeader(header);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestSerializeParseRoundTrip() {
    const auto header = MakeIpcHeader(IpcMessageType::FactsSnapshot, 1234, 0x01020304);
    std::array<std::byte, kIpcHeaderSize> bytes{};
    SerializeHeader(header, bytes);
    auto parsed = ParseHeader(bytes);
    if (!parsed) {
        return false;
    }
    const auto& value = parsed.Value();
    return value.magic == header.magic && value.version == header.version &&
           value.type == header.type && value.reserved == header.reserved &&
           value.payloadLength == header.payloadLength &&
           value.requestId == header.requestId;
}

bool TestParseHeaderRejectsWrongSize() {
    std::array<std::byte, kIpcHeaderSize> bytes{};
    SerializeHeader(MakeIpcHeader(IpcMessageType::Ping, 0, 1), bytes);
    return !ParseHeader(std::span<const std::byte>(bytes.data(), 15)) &&
           !ParseHeader(std::span<const std::byte>(bytes.data(), 17));
}

bool TestParseHeaderRejectsCorruptBytes() {
    std::array<std::byte, kIpcHeaderSize> bytes{};
    SerializeHeader(MakeIpcHeader(IpcMessageType::Ping, 0, 1), bytes);

    auto corrupted = bytes;
    corrupted[0] = std::byte{0x00}; // 破坏魔数
    if (ParseHeader(std::span<const std::byte>(corrupted))) {
        return false;
    }
    corrupted = bytes;
    corrupted[4] = std::byte{0x02}; // 未知版本
    if (ParseHeader(std::span<const std::byte>(corrupted))) {
        return false;
    }
    corrupted = bytes;
    corrupted[5] = std::byte{0x42}; // 未知类型
    return !ParseHeader(std::span<const std::byte>(corrupted));
}

// ---------- Facts 载荷契约（CPOPFACTS/1，IPC-002） ----------

// 事实列表逐项相等（顺序敏感）。
bool FactsEqual(const std::vector<IpcFact>& a, const std::vector<IpcFact>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].key != b[i].key || a[i].value != b[i].value) {
            return false;
        }
    }
    return true;
}

bool TestFactsSerializeParseRoundTrip() {
    std::vector<IpcFact> facts;
    facts.push_back({"client_pid", "12345"});
    facts.push_back({"observer", "CppOptimizer ipc demo"});
    facts.push_back({"note", "hello world"});
    facts.push_back({"empty", ""});
    auto encoded = SerializeFactsV1(facts);
    if (!encoded) {
        return false;
    }
    const auto& bytes = encoded.Value();
    // 首行必须是信封 + LF。
    const std::string prefix = "CPOPFACTS/1\n";
    if (bytes.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (bytes[i] !=
            static_cast<std::byte>(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    auto parsed = ParseFactsV1(bytes);
    return parsed && FactsEqual(parsed.Value(), facts);
}

bool TestFactsUnicodeValueRoundTrip() {
    // 值允许多字节 UTF-8（内存 = 0xE5 0x86 0x85 0xE5 0xAD 0x98）。
    std::vector<IpcFact> facts;
    facts.push_back({"module", "\xE5\x86\x85\xE5\xAD\x98"});
    auto encoded = SerializeFactsV1(facts);
    if (!encoded) {
        return false;
    }
    auto parsed = ParseFactsV1(encoded.Value());
    return parsed && parsed.Value().size() == 1 &&
           parsed.Value()[0].key == "module" &&
           parsed.Value()[0].value ==
               std::string("\xE5\x86\x85\xE5\xAD\x98", 6);
}

bool TestFactsEnvelopeOnlyIsEmptyFacts() {
    auto parsed = ParseFactsV1(BytesFromText("CPOPFACTS/1"));
    return parsed && parsed.Value().empty();
}

bool TestFactsRejectsEmptyPayload() {
    const auto parsed = ParseFactsV1(std::span<const std::byte>());
    return !parsed && parsed.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsRejectsMissingOrWrongEnvelope() {
    const char* cases[] = {
        "a=1",                   // 首行不是信封
        "facts clientPid=12345", // IPC-001 旧演示载荷：无信封，整体拒绝
        "CPOPFACTS/2\na=1",      // 载荷版本不匹配
        "CPOPFACTS\na=1",        // 非完整信封
        "NOTFACTS/1\na=1",
    };
    for (const char* text : cases) {
        const auto parsed = ParseFactsV1(BytesFromText(text));
        if (parsed || parsed.ErrorValue().domain != ErrorDomain::Validation) {
            return false;
        }
    }
    return true;
}

bool TestFactsRejectsTrailingNewlineOrBlankLine() {
    const char* cases[] = {
        "CPOPFACTS/1\n",        // 尾随换行
        "CPOPFACTS/1\na=1\n",   // 尾随换行
        "CPOPFACTS/1\n\na=1",   // 空行
        "\nCPOPFACTS/1\na=1",   // 前导空行
    };
    for (const char* text : cases) {
        const auto parsed = ParseFactsV1(BytesFromText(text));
        if (parsed || parsed.ErrorValue().domain != ErrorDomain::Validation) {
            return false;
        }
    }
    return true;
}

bool TestFactsRejectsCarriageReturn() {
    const char* cases[] = {
        "CPOPFACTS/1\r\na=1", // CRLF
        "CPOPFACTS/1\na=1\rb=2", // 孤立 CR
    };
    for (const char* text : cases) {
        const auto parsed = ParseFactsV1(BytesFromText(text));
        if (parsed || parsed.ErrorValue().domain != ErrorDomain::Validation) {
            return false;
        }
    }
    return true;
}

bool TestFactsRejectsMissingEquals() {
    const auto parsed = ParseFactsV1(BytesFromText("CPOPFACTS/1\nab"));
    return !parsed && parsed.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsRejectsEmptyKey() {
    const auto parsed = ParseFactsV1(BytesFromText("CPOPFACTS/1\n=1"));
    return !parsed && parsed.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsRejectsIllegalKeyCharacters() {
    // 点、空格与多字节字符都不属于键字符集。
    const std::string cases[] = {
        "CPOPFACTS/1\ndot.key=1",
        "CPOPFACTS/1\na b=1",
        "CPOPFACTS/1\n\xE4\xBD\xA0\xE5\xA5\xBD=1", // 你好=1
    };
    for (const std::string& text : cases) {
        const auto parsed = ParseFactsV1(BytesFromText(text));
        if (parsed || parsed.ErrorValue().domain != ErrorDomain::Validation) {
            return false;
        }
    }
    return true;
}

bool TestFactsRejectsDuplicateKey() {
    const auto parsed = ParseFactsV1(BytesFromText("CPOPFACTS/1\na=1\na=2"));
    return !parsed && parsed.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsRejectsControlBytes() {
    // 控制字节（\x01、\t、\x7F DEL）与值格式冲突，整体拒绝。
    const std::string cases[] = {
        std::string("CPOPFACTS/1\na=") + "\x01",
        std::string("CPOPFACTS/1\na=1\t"),
        std::string("CPOPFACTS/1\na=") + "\x7F",
    };
    for (const std::string& text : cases) {
        const auto parsed = ParseFactsV1(BytesFromText(text));
        if (parsed || parsed.ErrorValue().domain != ErrorDomain::Validation) {
            return false;
        }
    }
    return true;
}

bool TestFactsRejectsOversizedKeyOrValue() {
    // 键超上限字节拒绝。
    std::string longKeyText = "CPOPFACTS/1\n";
    longKeyText += std::string(kMaxFactsKeyBytes + 1, 'k');
    longKeyText += "=1";
    const auto longKey = ParseFactsV1(BytesFromText(longKeyText));
    if (longKey || longKey.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    // 值超上限字节拒绝。
    std::string longValueText = "CPOPFACTS/1\na=";
    longValueText += std::string(kMaxFactsValueBytes + 1, 'v');
    const auto longValue = ParseFactsV1(BytesFromText(longValueText));
    return !longValue &&
           longValue.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsRejectsTooManyEntries() {
    // 超过 kMaxFactsEntries 条拒绝。
    std::string text = "CPOPFACTS/1";
    for (std::size_t i = 0; i <= kMaxFactsEntries; ++i) {
        text += "\nk" + std::to_string(i) + "=1";
    }
    const auto parsed = ParseFactsV1(BytesFromText(text));
    if (parsed || parsed.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    // 编码同样拒绝。
    std::vector<IpcFact> facts;
    for (std::size_t i = 0; i <= kMaxFactsEntries; ++i) {
        facts.push_back({"k" + std::to_string(i), "1"});
    }
    const auto encoded = SerializeFactsV1(facts);
    return !encoded &&
           encoded.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsSerializeRejectsInvalidInput() {
    std::vector<IpcFact> badKey = {{"bad!key", "1"}};
    if (SerializeFactsV1(badKey)) {
        return false;
    }
    std::vector<IpcFact> badValue = {{"a", "line1\nline2"}};
    if (SerializeFactsV1(badValue)) {
        return false;
    }
    std::vector<IpcFact> controlValue = {{"a", "\x01"}};
    if (SerializeFactsV1(controlValue)) {
        return false;
    }
    std::vector<IpcFact> dup = {{"a", "1"}, {"a", "2"}};
    if (SerializeFactsV1(dup)) {
        return false;
    }
    std::vector<IpcFact> oversized = {{"a", std::string(kMaxFactsValueBytes + 1, 'v')}};
    return !SerializeFactsV1(oversized);
}

bool TestFactsSummaryEmptyAndBasic() {
    if (FormatFactsSummary({}) != "facts ok (0)") {
        return false;
    }
    const std::vector<IpcFact> facts = {{"a", "1"}, {"b", "hello world"}};
    return FormatFactsSummary(facts) == "facts ok (2): a=1 b=hello world";
}

bool TestFactsSummaryTruncationBounded() {
    std::vector<IpcFact> many;
    for (int i = 0; i < 30; ++i) {
        many.push_back({"k" + std::to_string(i), std::string(100, 'x')});
    }
    const std::string summary = FormatFactsSummary(many);
    const std::string suffix = " …";
    return summary.rfind("facts ok (30):", 0) == 0 &&
           summary.size() <= kFactsSummaryMaxBytes &&
           summary.size() >= suffix.size() &&
           summary.compare(summary.size() - suffix.size(), suffix.size(),
                           suffix) == 0;
}

bool TestFactsSchemaAcceptsValidSets() {
    const std::vector<IpcFact> singlePid = {{"client_pid", "12345"}};
    if (!ValidateFactsV1Schema(singlePid)) {
        return false;
    }
    const std::vector<IpcFact> memorySet = {
        {"memory_total_mb", "16384"},
        {"memory_available_mb", "8192"},
        {"memory_load_percent", "50"},
        {"observer", "demo"}};
    if (!ValidateFactsV1Schema(memorySet)) {
        return false;
    }
    // 边界合法：total=1、available==total、load=100。
    const std::vector<IpcFact> boundary = {
        {"memory_total_mb", "1"}, {"memory_available_mb", "1"},
        {"memory_load_percent", "100"}};
    if (!ValidateFactsV1Schema(boundary)) {
        return false;
    }
    // 前导零可接受（数值语义）。
    const std::vector<IpcFact> leadingZero = {{"client_pid", "007"}};
    return static_cast<bool>(ValidateFactsV1Schema(leadingZero));
}

bool TestFactsSchemaRejectsEmpty() {
    const auto result = ValidateFactsV1Schema({});
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsSchemaRejectsUnknownKey() {
    // 未知键与尚未注册的真实候选键（cpu_percent 不在 v1 白名单）整体拒绝。
    const std::vector<IpcFact> unknown = {{"hacker_key", "1"}};
    if (ValidateFactsV1Schema(unknown)) {
        return false;
    }
    const std::vector<IpcFact> unregistered = {{"cpu_percent", "50"}};
    return !ValidateFactsV1Schema(unregistered);
}

bool TestFactsSchemaRejectsNonNumericValue() {
    const std::vector<IpcFact> cases[] = {
        {{"client_pid", "abc"}}, {{"memory_total_mb", "-1"}},
        {{"memory_total_mb", "12.5"}}, {{"client_pid", ""}},
        {{"memory_available_mb", " 100"}}};
    for (const auto& facts : cases) {
        if (ValidateFactsV1Schema(facts)) {
            return false;
        }
    }
    return true;
}

bool TestFactsSchemaRejectsOutOfRange() {
    const std::vector<IpcFact> zeroTotal = {{"memory_total_mb", "0"}};
    if (ValidateFactsV1Schema(zeroTotal)) {
        return false;
    }
    const std::vector<IpcFact> load101 = {{"memory_load_percent", "101"}};
    if (ValidateFactsV1Schema(load101)) {
        return false;
    }
    const std::vector<IpcFact> overflowPid = {{"client_pid", "4294967296"}};
    // 4294967296 超出 uint32。
    return !ValidateFactsV1Schema(overflowPid);
}

bool TestFactsSchemaRejectsAvailableGtTotal() {
    const std::vector<IpcFact> facts = {{"memory_total_mb", "1000"},
                                        {"memory_available_mb", "1001"}};
    const auto result = ValidateFactsV1Schema(facts);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsSchemaRejectsEmptyObserver() {
    const std::vector<IpcFact> facts = {{"observer", ""}};
    const auto result = ValidateFactsV1Schema(facts);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestFactsSchemaTokenRules() {
    // 合法：凭据 + 至少一条非凭据事实。
    const std::vector<IpcFact> valid = {{"agent_token", "secret123"},
                                        {"client_pid", "5"}};
    if (!ValidateFactsV1Schema(valid)) {
        return false;
    }
    // 空凭据、超长凭据、非法字符凭据整体拒绝。
    const std::vector<IpcFact> emptyToken = {{"agent_token", ""},
                                              {"client_pid", "5"}};
    if (ValidateFactsV1Schema(emptyToken)) {
        return false;
    }
    const std::vector<IpcFact> longToken = {
        {"agent_token", std::string(65, 'a')}, {"client_pid", "5"}};
    if (ValidateFactsV1Schema(longToken)) {
        return false;
    }
    const std::vector<IpcFact> badChars = {{"agent_token", "abc!1"},
                                           {"client_pid", "5"}};
    if (ValidateFactsV1Schema(badChars)) {
        return false;
    }
    // 只有凭据、无非凭据事实：拒绝（凭据不满足“至少一条”）。
    const std::vector<IpcFact> onlyToken = {{"agent_token", "secret123"}};
    return !ValidateFactsV1Schema(onlyToken);
}

bool TestFactsTokenExcludedFromSummary() {
    const std::vector<IpcFact> facts = {{"agent_token", "topsecret"},
                                        {"client_pid", "7"},
                                        {"observer", "x"}};
    // 凭据不回显且不计入摘要条数（防泄漏）。
    return FormatFactsSummary(facts) == "facts ok (2): client_pid=7 observer=x";
}

// ---------- 凭据存储（IPC-007 真实供给） ----------

// 临时 token 文件路径（先清理再使用）。
std::filesystem::path TempTokenFile(std::wstring_view name) noexcept {
    auto path = std::filesystem::temp_directory_path();
    path /= std::wstring(L"cpo_token_") + std::wstring(name) + L".bin";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

bool TestCredentialsProvisionReadRoundTrip() {
    const auto path = TempTokenFile(L"roundtrip");
    auto provisioned = optimizer::ipc::ProvisionAgentTokenFile(path, false);
    if (!provisioned) {
        return false;
    }
    if (!optimizer::ipc::IsAgentTokenProvisioned(path)) {
        return false;
    }
    auto token = optimizer::ipc::ReadAgentTokenFile(path);
    if (!token || token.Value().size() != 32) {
        return false;
    }
    // 字符集合法（ASCII 字母/数字）。
    for (const wchar_t ch : token.Value()) {
        const bool alnum =
            (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9');
        if (!alnum) {
            return false;
        }
    }
    return true;
}

bool TestCredentialsProvisionIdempotent() {
    const auto path = TempTokenFile(L"idempotent");
    if (!optimizer::ipc::ProvisionAgentTokenFile(path, false)) {
        return false;
    }
    auto first = optimizer::ipc::ReadAgentTokenFile(path);
    if (!first) {
        return false;
    }
    // 已存在且未 force：拒绝且不覆盖。
    const auto again = optimizer::ipc::ProvisionAgentTokenFile(path, false);
    if (again || again.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    auto second = optimizer::ipc::ReadAgentTokenFile(path);
    return second && second.Value() == first.Value();
}

bool TestCredentialsForceRotationChangesToken() {
    const auto path = TempTokenFile(L"rotate");
    if (!optimizer::ipc::ProvisionAgentTokenFile(path, false)) {
        return false;
    }
    auto before = optimizer::ipc::ReadAgentTokenFile(path);
    if (!before) {
        return false;
    }
    if (!optimizer::ipc::ProvisionAgentTokenFile(path, true)) {
        return false;
    }
    auto after = optimizer::ipc::ReadAgentTokenFile(path);
    return after && after.Value() != before.Value() &&
           after.Value().size() == before.Value().size();
}

bool TestCredentialsRejectsInvalidContent() {
    const auto path = TempTokenFile(L"invalid");
    // 直接写入非法内容（非字母数字）。
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        const char junk[] = "not-a-valid-token!";
        DWORD written = 0;
        const BOOL ok =
            ::WriteFile(file, junk, static_cast<DWORD>(sizeof(junk) - 1),
                        &written, nullptr);
        ::CloseHandle(file);
        if (!ok) {
            return false;
        }
    }
    if (optimizer::ipc::IsAgentTokenProvisioned(path)) {
        return false;
    }
    const auto token = optimizer::ipc::ReadAgentTokenFile(path);
    return !token && token.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestCredentialsDaclRestrictedToSystemAndUser() {
    const auto path = TempTokenFile(L"dacl");
    if (!optimizer::ipc::ProvisionAgentTokenFile(path, false)) {
        return false;
    }
    // 读取 DACL 的 SDDL 文本并断言：保护 + SYSTEM + 当前用户；无 Everyone。
    DWORD needed = 0;
    (void)::GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr,
                             0, &needed);
    if (needed == 0) {
        return false;
    }
    std::vector<std::byte> buffer(needed);
    if (!::GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                            buffer.data(), static_cast<DWORD>(buffer.size()),
                            &needed)) {
        return false;
    }
    LPWSTR text = nullptr;
    if (!::ConvertSecurityDescriptorToStringSecurityDescriptorW(
            buffer.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text,
            nullptr)) {
        return false;
    }
    const std::wstring sddl = text;
    ::LocalFree(text);
    return sddl.find(L"D:P") == 0 && sddl.find(L";SY)") != std::wstring::npos &&
           sddl.find(L";WD)") == std::wstring::npos &&
           sddl.find(L"S-1-") != std::wstring::npos;
}

// ---------- 服务端会话（fake 后端） ----------

bool TestServeOnePingAck() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served) {
        return false;
    }
    const auto& result = served.Value();
    if (result.requestType != IpcMessageType::Ping ||
        result.requestId != 7 || result.clientPid != 1234 ||
        result.clientSessionId != 2 || result.payloadBytes != 0) {
        return false;
    }
    // 应答帧必须是 Ack 且 requestId 配对。
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           header.requestId == 7 && payload.empty();
}

bool TestServeOneFactsSnapshotAck() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    // 结构化事实载荷（CPOPFACTS/1）。
    std::vector<IpcFact> facts;
    facts.push_back({"client_pid", "12345"});
    facts.push_back({"observer", "demo"});
    auto encoded = SerializeFactsV1(facts);
    if (!encoded) {
        return false;
    }
    const auto payload = encoded.Value();
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().requestType !=
                       IpcMessageType::FactsSnapshot ||
        served.Value().replyType != IpcMessageType::Ack ||
        served.Value().payloadBytes != payload.size()) {
        return false;
    }
    // 应答必须是 Ack，载荷为紧凑摘要文本（与服务端解析结果一致）。
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    const auto expected = BytesFromText(FormatFactsSummary(facts));
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           header.requestId == 3 && replyPayload == expected;
}

bool TestServeOneInvalidFactsReply() {
    // 载荷违反 CPOPFACTS/1（旧演示文本、无信封）：默认处理器回 Error(InvalidFacts)。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload = BytesFromText("facts clientPid=12345");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Error ||
        served.Value().payloadBytes != payload.size()) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::InvalidFacts;
}

bool TestServeOneUnknownFactsKeyReply() {
    // 语法合法但含白名单外键（hacker_key）：键语义校验拒绝 -> Error(InvalidFacts)。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload = BytesFromText("CPOPFACTS/1\nhacker_key=1");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Error ||
        served.Value().payloadBytes != payload.size()) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::InvalidFacts;
}

bool TestServeOneEmptyFactsReply() {
    // 语法合法（仅信封行）但 schema 要求至少一条已注册事实：拒绝。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload = BytesFromText("CPOPFACTS/1");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Error ||
        served.Value().payloadBytes != payload.size()) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::InvalidFacts;
}

bool TestSessionGateRejectsSessionZero() {
    // 会话 0（服务/非交互）不是合法 Agent：默认裁决拒绝并回 Error(UnauthorizedClient)。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 0;
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::UnauthorizedClient;
}

bool TestSessionGateRejectsUnknownPid() {
    // PID 不可识别（查询失败为 0）：默认裁决拒绝。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 0;
    fake->clientSessionId = 2;
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::UnauthorizedClient;
}

bool TestSessionGateCustomAllowOverride() {
    // 显式放行裁决可覆盖默认（会话 0 也放行）——隔离/测试场景使用。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 0;
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession::Options options;
    options.clientGate = IpcSession::AllowAllClientGate;
    IpcSession session(fake, options);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Ack) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack);
}

bool TestSessionGateCustomReject() {
    // 自定义裁决：可按任意身份规则拒绝（身份白名单扩展点）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 4242;
    fake->clientSessionId = 2;
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession::Options options;
    options.clientGate = [](const IpcClientIdentity& client) -> Result<void> {
        if (client.pid == 4242) {
            return Result<void>::Failure(
                Error::Validation("gate", L"pid 不在白名单"));
        }
        return Result<void>::Success();
    };
    IpcSession session(fake, options);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::UnauthorizedClient;
}

bool TestServeOneCustomHandler() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 9, {});
    IpcSession session(fake);
    auto served = session.ServeOne(
        [](const IpcRequest& request, IpcReply& reply) {
            reply.type = IpcMessageType::Ack;
            reply.payload = BytesFromText("pong " + std::to_string(request.clientPid));
            return Result<void>::Success();
        },
        std::chrono::milliseconds(100));
    if (!served) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           replyPayload.size() == 9; // "pong 1234"
}

bool TestServeOneBadMagicRejected() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    auto frame = BuildFrame(IpcMessageType::Ping, 1, {});
    frame[0] = std::byte{0x00}; // 破坏魔数
    fake->incoming = std::move(frame);
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served) {
        return false;
    }
    if (served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    // 必须写出 Error 应答（InvalidHeader）。
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !payload.empty() &&
           static_cast<IpcErrorCode>(payload[0]) == IpcErrorCode::InvalidHeader;
}

bool TestServeOneUnknownVersionRejected() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    auto frame = BuildFrame(IpcMessageType::Ping, 1, {});
    frame[4] = std::byte{0x02}; // 未知版本
    fake->incoming = std::move(frame);
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           static_cast<IpcErrorCode>(payload[0]) == IpcErrorCode::InvalidHeader;
}

bool TestServeOneUnknownTypeRejected() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    auto frame = BuildFrame(IpcMessageType::Ping, 1, {});
    frame[5] = std::byte{0x42}; // 未知类型
    fake->incoming = std::move(frame);
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return !served &&
           served.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestServeOneOversizedPayloadRejected() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    auto frame = BuildFrame(IpcMessageType::Ping, 1, {});
    // 直接把帧头 payloadLength 改为超限值（不真实发送超长载荷，帧级校验即拦截）。
    const auto oversized = MakeIpcHeader(IpcMessageType::Ping,
                                         kMaxPayloadLength + 1, 1);
    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    SerializeHeader(oversized, headerBytes);
    std::copy(headerBytes.begin(), headerBytes.end(), frame.begin());
    fake->incoming = std::move(frame);
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error);
}

bool TestServeOneAcceptTimeout() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->failAccept = true;
    fake->acceptErrorCode = ERROR_TIMEOUT;
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(0));
    return !served && served.ErrorValue().domain == ErrorDomain::Win32 &&
           served.ErrorValue().code == ERROR_TIMEOUT;
}

bool TestServeOneCreateFailure() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->failCreate = true;
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return !served && served.ErrorValue().domain == ErrorDomain::Win32 &&
           served.ErrorValue().code == ERROR_ACCESS_DENIED;
}

bool TestServeOneReadFailureNotDisguised() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->failRead = true;
    fake->failErrorCode = ERROR_BROKEN_PIPE;
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return !served && served.ErrorValue().domain == ErrorDomain::Win32 &&
           served.ErrorValue().code == ERROR_BROKEN_PIPE;
}

bool TestServeOneWriteFailureNotDisguised() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 1, {});
    fake->failWrite = true;
    fake->failErrorCode = ERROR_NO_DATA;
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return !served && served.ErrorValue().domain == ErrorDomain::Win32 &&
           served.ErrorValue().code == ERROR_NO_DATA;
}

bool TestServeOneHandlerFailureSendsError() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 5, {});
    IpcSession session(fake);
    const auto served = session.ServeOne(
        [](const IpcRequest&, IpcReply&) {
            return Result<void>::Failure(Error::Validation(
                "handler", L"拒绝"));
        },
        std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           static_cast<IpcErrorCode>(payload[0]) == IpcErrorCode::HandlerFailed;
}

bool TestServeOneDefaultHandlerRejectsRequestType() {
    // 默认处理器只接受 Ping/FactsSnapshot；Ack 作为请求应被拒绝（UnsupportedType）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ack, 5, {});
    IpcSession session(fake);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           static_cast<IpcErrorCode>(payload[0]) ==
               IpcErrorCode::UnsupportedType;
}

bool TestSessionSidGateAllowsMatchingUser() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 2;
    fake->clientSid = L"S-1-5-21-111-222-333-1001";
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession::Options options;
    options.allowedClientSids.push_back(L"s-1-5-21-111-222-333-1001"); // 大小写不敏感
    IpcSession session(fake, options);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Ack ||
        served.Value().clientUserSid != L"S-1-5-21-111-222-333-1001") {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack);
}

bool TestSessionSidGateRejectsUnknownSid() {
    // 启用授权但客户端 SID 查询失败（未知）：拒绝并回 Error(UnauthorizedClient)。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 2;
    fake->clientSid = L"";
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession::Options options;
    options.allowedClientSids.push_back(L"S-1-5-21-111-222-333-1001");
    IpcSession session(fake, options);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::UnauthorizedClient;
}

bool TestSessionSidGateRejectsMismatch() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 2;
    fake->clientSid = L"S-1-5-21-111-222-333-9999";
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession::Options options;
    options.allowedClientSids.push_back(L"S-1-5-21-111-222-333-1001");
    IpcSession session(fake, options);
    const auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::UnauthorizedClient;
}

bool TestSessionSidGateDisabledWhenEmpty() {
    // allowedClientSids 为空：SID 授权不启用（即使未知 SID 也放行，兼容 IPC-004/005）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 2;
    fake->clientSid = L"";
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return served && served.Value().replyType == IpcMessageType::Ack;
}

bool TestServePersistentReusesInstanceAcrossClients() {
    // 持续受理：同一实例连续服务两个客户端，ServeOne 之间不关闭（无监听空窗）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    IpcSession::Options options;
    options.persistentAccept = true;
    IpcSession session(fake, options);

    auto first = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!first || first.Value().replyType != IpcMessageType::Ack) {
        return false;
    }
    if (fake->closeCount != 0 || fake->disconnectCount != 1) {
        return false; // 服务结束应断开当前客户端但保留实例
    }
    // 第二个客户端（追加帧模拟新连接，fake 以读偏移+断管边界区分）。
    const auto payload = BytesFromText("CPOPFACTS/1\nobserver=demo");
    const auto secondFrame =
        BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    fake->incoming.insert(fake->incoming.end(), secondFrame.begin(),
                          secondFrame.end());
    auto second =
        session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!second || second.Value().replyType != IpcMessageType::Ack) {
        return false;
    }
    if (fake->acceptCount != 2 || fake->closeCount != 0) {
        return false;
    }
    session.Close(); // 调用方结束持续会话
    return fake->closeCount == 1;
}

bool TestServePersistentAcceptTimeoutKeepsInstance() {
    // 持续模式接受超时：保留监听实例（返回 ERROR_TIMEOUT，不关闭）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->failAccept = true;
    fake->acceptErrorCode = ERROR_TIMEOUT;
    IpcSession::Options options;
    options.persistentAccept = true;
    IpcSession session(fake, options);
    const auto served =
        session.ServeOne(nullptr, std::chrono::milliseconds(0));
    if (served || served.ErrorValue().domain != ErrorDomain::Win32 ||
        served.ErrorValue().code != ERROR_TIMEOUT || fake->closeCount != 0) {
        return false;
    }
    session.Close();
    return fake->closeCount == 1;
}

bool TestServeNonPersistentAcceptTimeoutCloses() {
    // 默认（非持续）语义不回归：接受超时关闭实例。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->failAccept = true;
    fake->acceptErrorCode = ERROR_TIMEOUT;
    IpcSession session(fake);
    const auto served =
        session.ServeOne(nullptr, std::chrono::milliseconds(0));
    return !served && fake->closeCount == 1;
}

// ---------- 同一连接多帧会话（IPC-008，ServeSession） ----------

bool TestServeSessionTwoPingsOneConnection() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto first = BuildFrame(IpcMessageType::Ping, 1, {});
    const auto second = BuildFrame(IpcMessageType::Ping, 2, {});
    fake->incoming.insert(fake->incoming.end(), first.begin(), first.end());
    fake->incoming.insert(fake->incoming.end(), second.begin(), second.end());
    IpcSession session(fake);
    auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(200),
        std::chrono::milliseconds(5000));
    if (!served || served.Value().framesServed != 2 ||
        served.Value().endReason != IpcSessionEndReason::ClientClosed ||
        served.Value().replyType != IpcMessageType::Ack ||
        served.Value().requestId != 2) {
        return false;
    }
    // 同一连接上两帧：仅一次 accept/断开/关闭，逐帧 Ack 且 requestId 配对。
    std::vector<IpcHeader> headers;
    std::vector<std::vector<std::byte>> payloads;
    return ParseWrittenFrames(fake->outgoing, headers, payloads) &&
           headers.size() == 2 && fake->acceptCount == 1 &&
           fake->disconnectCount == 1 && fake->closeCount == 1 &&
           headers[0].type ==
               static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           headers[0].requestId == 1 &&
           headers[1].type ==
               static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           headers[1].requestId == 2;
}

bool TestServeSessionFactsTwoFrames() {
    auto fake = std::make_shared<FakeIpcServerBackend>();
    std::vector<IpcFact> facts = {{"client_pid", "12345"},
                                  {"observer", "demo"}};
    auto encoded = SerializeFactsV1(facts);
    if (!encoded) {
        return false;
    }
    const auto frame1 =
        BuildFrame(IpcMessageType::FactsSnapshot, 10, encoded.Value());
    const auto frame2 =
        BuildFrame(IpcMessageType::FactsSnapshot, 11, encoded.Value());
    fake->incoming.insert(fake->incoming.end(), frame1.begin(), frame1.end());
    fake->incoming.insert(fake->incoming.end(), frame2.begin(), frame2.end());
    IpcSession session(fake);
    auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(200),
        std::chrono::milliseconds(5000));
    if (!served || served.Value().framesServed != 2 ||
        served.Value().payloadBytes != encoded.Value().size()) {
        return false;
    }
    std::vector<IpcHeader> headers;
    std::vector<std::vector<std::byte>> payloads;
    if (!ParseWrittenFrames(fake->outgoing, headers, payloads) ||
        headers.size() != 2) {
        return false;
    }
    const auto expected = BytesFromText(FormatFactsSummary(facts));
    return headers[0].type ==
               static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           payloads[0] == expected && headers[1].requestId == 11 &&
           payloads[1] == expected;
}

bool TestServeSessionIdleTimeoutEnds() {
    // 帧间空闲超时（客户端静默超过 idleTimeout，心跳丢失）正常结束会话。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    fake->failReadAtCall = 2; // 服务完首帧后，读下一帧头超时
    fake->failReadAtErrorCode = ERROR_TIMEOUT;
    IpcSession session(fake);
    auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(50),
        std::chrono::milliseconds(5000));
    return served && served.Value().framesServed == 1 &&
           served.Value().endReason == IpcSessionEndReason::IdleTimeout &&
           served.Value().replyType == IpcMessageType::Ack &&
           fake->closeCount == 1;
}

bool TestServeSessionBudgetEnds() {
    // 会话总预算比帧间空闲先耗尽：按 SessionBudget 正常结束。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    fake->failReadAtCall = 2;
    fake->failReadAtErrorCode = ERROR_TIMEOUT;
    IpcSession session(fake);
    auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100),
        std::chrono::milliseconds(10000), std::chrono::milliseconds(2000));
    return served && served.Value().framesServed == 1 &&
           served.Value().endReason == IpcSessionEndReason::SessionBudget;
}

bool TestServeSessionInvalidSecondFrameEnds() {
    // 会话中第二帧非法（破坏魔数）：帧级严格拒绝 -> Error(InvalidHeader) 并终止。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto first = BuildFrame(IpcMessageType::Ping, 1, {});
    auto bad = BuildFrame(IpcMessageType::Ping, 2, {});
    bad[0] = std::byte{0x00};
    fake->incoming.insert(fake->incoming.end(), first.begin(), first.end());
    fake->incoming.insert(fake->incoming.end(), bad.begin(), bad.end());
    IpcSession session(fake);
    const auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(200),
        std::chrono::milliseconds(5000));
    if (served || served.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    std::vector<IpcHeader> headers;
    std::vector<std::vector<std::byte>> payloads;
    return ParseWrittenFrames(fake->outgoing, headers, payloads) &&
           headers.size() == 2 &&
           headers[0].type ==
               static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           headers[1].type ==
               static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !payloads[1].empty() &&
           static_cast<IpcErrorCode>(payloads[1][0]) ==
               IpcErrorCode::InvalidHeader;
}

bool TestServeSessionTokenCheckedEveryFrame() {
    // 会话每帧独立过凭据校验：首帧带 token 回 Ack，次帧缺 token 回
    // Error(AuthFailed)。Error 应答是合法回执，会话继续直至客户端关闭。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto withToken =
        BytesFromText("CPOPFACTS/1\nagent_token=secret123\nobserver=demo");
    const auto withoutToken = BytesFromText("CPOPFACTS/1\nobserver=demo");
    const auto frame1 =
        BuildFrame(IpcMessageType::FactsSnapshot, 1, withToken);
    const auto frame2 =
        BuildFrame(IpcMessageType::FactsSnapshot, 2, withoutToken);
    fake->incoming.insert(fake->incoming.end(), frame1.begin(), frame1.end());
    fake->incoming.insert(fake->incoming.end(), frame2.begin(), frame2.end());
    IpcSession::Options options;
    options.expectedToken = L"secret123";
    IpcSession session(fake, options);
    auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(200),
        std::chrono::milliseconds(5000));
    if (!served || served.Value().framesServed != 2) {
        return false;
    }
    std::vector<IpcHeader> headers;
    std::vector<std::vector<std::byte>> payloads;
    return ParseWrittenFrames(fake->outgoing, headers, payloads) &&
           headers.size() == 2 &&
           headers[0].type ==
               static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           headers[1].type ==
               static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !payloads[1].empty() &&
           static_cast<IpcErrorCode>(payloads[1][0]) ==
               IpcErrorCode::AuthFailed;
}

bool TestServeSessionConnectThenCloseIsFailure() {
    // 客户端连接后未发任何帧即关闭：0 帧不伪装成功（与单帧语义一致）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    IpcSession session(fake);
    const auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(200),
        std::chrono::milliseconds(5000));
    return !served && served.ErrorValue().domain == ErrorDomain::Win32 &&
           served.ErrorValue().code == ERROR_BROKEN_PIPE &&
           fake->closeCount == 1;
}

bool TestServeSessionZeroParamsRejected() {
    // 空闲超时/会话预算必须为正（Validation 拒绝，不触后端）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    IpcSession session(fake);
    const auto served = session.ServeSession(
        nullptr, std::chrono::milliseconds(100), std::chrono::milliseconds(0),
        std::chrono::milliseconds(0));
    return !served && served.ErrorValue().domain == ErrorDomain::Validation &&
           fake->createCount == 0;
}

// ---------- 受理结论观察者（IPC-010，Safe Mode 联动） ----------

bool TestVerdictObserverAcceptedOnServe() {
    // 正常受理完成：连接层面回调一次 Accepted。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    std::vector<IpcClientVerdict> verdicts;
    IpcSession::Options options;
    options.verdictObserver = [&verdicts](IpcClientVerdict v) {
        verdicts.push_back(v);
    };
    IpcSession session(fake, options);
    const auto served =
        session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return served && verdicts.size() == 1 &&
           verdicts[0] == IpcClientVerdict::Accepted;
}

bool TestVerdictObserverGateReject() {
    // 会话级身份裁决拒绝（会话 0）：回调 UnauthorizedClient（连接层面一次），不回调 Accepted。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientPid = 1234;
    fake->clientSessionId = 0;
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    std::vector<IpcClientVerdict> verdicts;
    IpcSession::Options options;
    options.verdictObserver = [&verdicts](IpcClientVerdict v) {
        verdicts.push_back(v);
    };
    IpcSession session(fake, options);
    const auto served =
        session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return !served && verdicts.size() == 1 &&
           verdicts[0] == IpcClientVerdict::UnauthorizedClient;
}

bool TestVerdictObserverSidReject() {
    // SID 白名单不匹配：回调 UnauthorizedClient。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->clientSid = L"S-1-5-21-1-2-3-9999";
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    std::vector<IpcClientVerdict> verdicts;
    IpcSession::Options options;
    options.allowedClientSids.push_back(L"S-1-5-21-1-2-3-1001");
    options.verdictObserver = [&verdicts](IpcClientVerdict v) {
        verdicts.push_back(v);
    };
    IpcSession session(fake, options);
    const auto served =
        session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return !served && verdicts.size() == 1 &&
           verdicts[0] == IpcClientVerdict::UnauthorizedClient;
}

bool TestVerdictObserverAuthFailed() {
    // 凭据不符（缺 token）：服务端回 Error(AuthFailed)；结论为 AuthFailed（非 Accepted）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload = BytesFromText("CPOPFACTS/1\nobserver=demo");
    fake->incoming =
        BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    std::vector<IpcClientVerdict> verdicts;
    IpcSession::Options options;
    options.expectedToken = L"secret123";
    options.verdictObserver = [&verdicts](IpcClientVerdict v) {
        verdicts.push_back(v);
    };
    IpcSession session(fake, options);
    const auto served =
        session.ServeOne(nullptr, std::chrono::milliseconds(100));
    return served &&
           served.Value().replyType == IpcMessageType::Error &&
           verdicts.size() == 1 &&
           verdicts[0] == IpcClientVerdict::AuthFailed;
}

// ---------- 客户端往返（fake 后端） ----------

bool TestServeTokenMatchingAck() {
    // 配置 expectedToken 且载荷携带完全匹配的 agent_token：回 Ack（摘要不含凭据）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    std::vector<IpcFact> facts = {{"agent_token", "secret123"},
                                  {"observer", "demo"}};
    auto encoded = SerializeFactsV1(facts);
    if (!encoded) {
        return false;
    }
    fake->incoming =
        BuildFrame(IpcMessageType::FactsSnapshot, 3, encoded.Value());
    IpcSession::Options options;
    options.expectedToken = L"secret123";
    IpcSession session(fake, options);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Ack) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           replyPayload == BytesFromText(FormatFactsSummary(facts));
}

bool TestServeTokenMissingRejected() {
    // 服务端要求凭据但载荷缺失：回 Error(AuthFailed)（不伪装成功）。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload = BytesFromText("CPOPFACTS/1\nobserver=demo");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession::Options options;
    options.expectedToken = L"secret123";
    IpcSession session(fake, options);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Error ||
        served.Value().payloadBytes != payload.size()) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::AuthFailed;
}

bool TestServeTokenMismatchRejected() {
    // 凭据不匹配：回 Error(AuthFailed)。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload = BytesFromText("CPOPFACTS/1\nagent_token=wrong\nobserver=demo");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession::Options options;
    options.expectedToken = L"secret123";
    IpcSession session(fake, options);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Error) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Error) &&
           !replyPayload.empty() &&
           static_cast<IpcErrorCode>(replyPayload[0]) ==
               IpcErrorCode::AuthFailed;
}

bool TestServeTokenIgnoredWhenNotConfigured() {
    // 服务端未配置 expectedToken：载荷中的 agent_token 仅作 schema 合法项、被忽略。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    const auto payload =
        BytesFromText("CPOPFACTS/1\nagent_token=secret123\nobserver=demo");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().replyType != IpcMessageType::Ack) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack);
}

bool TestRoundTripAuthFailedReplyNotDisguised() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const std::vector<std::byte> errorPayload{
        static_cast<std::byte>(IpcErrorCode::AuthFailed)};
    fake->incoming = BuildFrame(IpcMessageType::Error, 42, errorPayload);
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test",
                                    IpcMessageType::FactsSnapshot, {}, 42,
                                    std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Validation &&
           reply.ErrorValue().message.find(L"authentication failed") !=
               std::wstring::npos;
}


bool TestRoundTripPingAck() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ack, 42, {});
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test", IpcMessageType::Ping,
                                    {}, 42, std::chrono::milliseconds(100));
    if (!reply || reply.Value().type != IpcMessageType::Ack ||
        !reply.Value().payload.empty()) {
        return false;
    }
    if (fake->connectedPath != L"\\\\.\\pipe\\test" || !fake->closed) {
        return false;
    }
    // 请求帧：Ping + requestId 42。
    IpcHeader header;
    std::vector<std::byte> payload;
    return ParseWrittenFrame(fake->outgoing, header, payload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ping) &&
           header.requestId == 42 && payload.empty();
}

bool TestRoundTripFactsPayload() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const auto payload = BytesFromText("fact");
    fake->incoming = BuildFrame(IpcMessageType::Ack, 1, payload);
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test",
                                    IpcMessageType::FactsSnapshot, payload, 1,
                                    std::chrono::milliseconds(100));
    if (!reply || reply.Value().payload.size() != 4) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> requestPayload;
    return ParseWrittenFrame(fake->outgoing, header, requestPayload) &&
           header.type ==
               static_cast<std::uint8_t>(IpcMessageType::FactsSnapshot) &&
           requestPayload.size() == 4;
}

bool TestRoundTripErrorReplyNotDisguised() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const std::vector<std::byte> errorPayload{
        static_cast<std::byte>(IpcErrorCode::InvalidHeader)};
    fake->incoming = BuildFrame(IpcMessageType::Error, 42, errorPayload);
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test", IpcMessageType::Ping,
                                    {}, 42, std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestRoundTripUnauthorizedErrorReplyNotDisguised() {
    // 服务端身份裁决拒绝 -> Error(UnauthorizedClient)：客户端解析为失败且可见原因。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const std::vector<std::byte> errorPayload{
        static_cast<std::byte>(IpcErrorCode::UnauthorizedClient)};
    fake->incoming = BuildFrame(IpcMessageType::Error, 42, errorPayload);
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test",
                                    IpcMessageType::Ping, {}, 42,
                                    std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Validation &&
           reply.ErrorValue().message.find(L"unauthorized client") !=
               std::wstring::npos &&
           fake->closed;
}

bool TestRoundTripRequestIdMismatch() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ack, 99, {}); // 应答 id 不匹配
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test", IpcMessageType::Ping,
                                    {}, 42, std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Validation;
}

bool TestRoundTripConnectFailure() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->failConnect = true;
    fake->failCode = ERROR_FILE_NOT_FOUND;
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test", IpcMessageType::Ping,
                                    {}, 1, std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Win32 &&
           reply.ErrorValue().code == ERROR_FILE_NOT_FOUND;
}

bool TestRoundTripWriteFailure() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->failWrite = true;
    fake->failCode = ERROR_ACCESS_DENIED;
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test", IpcMessageType::Ping,
                                    {}, 1, std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Win32 &&
           reply.ErrorValue().code == ERROR_ACCESS_DENIED && fake->closed;
}

bool TestRoundTripReadFailure() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->failRead = true;
    fake->failCode = ERROR_BROKEN_PIPE;
    const auto reply = IpcRoundTrip(fake, L"\\\\.\\pipe\\test", IpcMessageType::Ping,
                                    {}, 1, std::chrono::milliseconds(100));
    return !reply && reply.ErrorValue().domain == ErrorDomain::Win32 &&
           reply.ErrorValue().code == ERROR_BROKEN_PIPE;
}

// ---------- 客户端多帧会话（IPC-008，IpcRoundTripSession） ----------

bool TestRoundTripSessionTwoPings() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const auto reply1 = BuildFrame(IpcMessageType::Ack, 1, {});
    const auto reply2 = BuildFrame(IpcMessageType::Ack, 2, {});
    fake->incoming.insert(fake->incoming.end(), reply1.begin(), reply1.end());
    fake->incoming.insert(fake->incoming.end(), reply2.begin(), reply2.end());
    std::vector<IpcFrameRequest> requests = {
        {IpcMessageType::Ping, 1, {}}, {IpcMessageType::Ping, 2, {}}};
    const auto result = IpcRoundTripSession(
        fake, L"\\.\\pipe\\test", requests, std::chrono::milliseconds(100));
    if (!result || result.Value().size() != 2) {
        return false;
    }
    // 连接一次，两帧请求与两帧应答一一配对。
    if (fake->connectedPath != L"\\.\\pipe\\test" || !fake->closed) {
        return false;
    }
    std::vector<IpcHeader> headers;
    std::vector<std::vector<std::byte>> payloads;
    return ParseWrittenFrames(fake->outgoing, headers, payloads) &&
           headers.size() == 2 && headers[0].requestId == 1 &&
           headers[1].requestId == 2;
}

bool TestRoundTripSessionEmptyNoConnect() {
    // 空请求序列：不连接、直接成功返回空列表（避免无意义的连接往返）。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const auto result =
        IpcRoundTripSession(fake, L"\\.\\pipe\\test", {},
                            std::chrono::milliseconds(100));
    return result && result.Value().empty() && !fake->closed &&
           fake->connectedPath.empty();
}

bool TestRoundTripSessionSecondErrorAborts() {
    // 会话中途 Error 应答：中止并关闭，不伪装成功，错误原因可见。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const auto ok = BuildFrame(IpcMessageType::Ack, 1, {});
    const std::vector<std::byte> errorPayload{
        static_cast<std::byte>(IpcErrorCode::AuthFailed)};
    const auto err = BuildFrame(IpcMessageType::Error, 2, errorPayload);
    fake->incoming.insert(fake->incoming.end(), ok.begin(), ok.end());
    fake->incoming.insert(fake->incoming.end(), err.begin(), err.end());
    std::vector<IpcFrameRequest> requests = {
        {IpcMessageType::Ping, 1, {}},
        {IpcMessageType::FactsSnapshot, 2, {}}};
    const auto result = IpcRoundTripSession(
        fake, L"\\.\\pipe\\test", requests, std::chrono::milliseconds(100));
    return !result && result.ErrorValue().domain == ErrorDomain::Validation &&
           result.ErrorValue().message.find(L"authentication failed") !=
               std::wstring::npos &&
           fake->closed;
}

bool TestRoundTripSessionMidRequestIdMismatch() {
    // 会话中途应答 requestId 与当前请求不配对：拒绝并中止。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    const auto ok = BuildFrame(IpcMessageType::Ack, 1, {});
    const auto bad = BuildFrame(IpcMessageType::Ack, 99, {}); // 不配对
    fake->incoming.insert(fake->incoming.end(), ok.begin(), ok.end());
    fake->incoming.insert(fake->incoming.end(), bad.begin(), bad.end());
    std::vector<IpcFrameRequest> requests = {
        {IpcMessageType::Ping, 1, {}}, {IpcMessageType::Ping, 2, {}}};
    const auto result = IpcRoundTripSession(
        fake, L"\\.\\pipe\\test", requests, std::chrono::milliseconds(100));
    return !result && result.ErrorValue().domain == ErrorDomain::Validation &&
           fake->closed;
}

bool TestRoundTripSessionConnectFailure() {
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->failConnect = true;
    fake->failCode = ERROR_PIPE_BUSY;
    std::vector<IpcFrameRequest> requests = {
        {IpcMessageType::Ping, 1, {}}, {IpcMessageType::Ping, 2, {}}};
    const auto result = IpcRoundTripSession(
        fake, L"\\.\\pipe\\test", requests, std::chrono::milliseconds(100));
    return !result && result.ErrorValue().domain == ErrorDomain::Win32 &&
           result.ErrorValue().code == ERROR_PIPE_BUSY;
}

// ---------- 多实例并发受理（IPC-009，RunConcurrentServer） ----------

bool TestRunConcurrentRejectsBadParams() {
    // 参数非法：Validation 拒绝且不启动任何 worker/工厂。
    int factoryCalls = 0;
    auto factory = [&factoryCalls]() -> std::shared_ptr<IpcSession> {
        ++factoryCalls;
        return std::make_shared<IpcSession>(
            std::make_shared<FakeIpcServerBackend>());
    };
    const auto badInstances = RunConcurrentServer(
        0, std::chrono::milliseconds(100), std::chrono::milliseconds(200),
        nullptr, factory);
    if (badInstances ||
        badInstances.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    const auto badWindow = RunConcurrentServer(
        2, std::chrono::milliseconds(0), std::chrono::milliseconds(200),
        nullptr, factory);
    if (badWindow || badWindow.ErrorValue().domain != ErrorDomain::Validation) {
        return false;
    }
    const auto badFactory =
        RunConcurrentServer(2, std::chrono::milliseconds(100),
                            std::chrono::milliseconds(200), nullptr, {});
    return !badFactory && factoryCalls == 0;
}

bool TestRunConcurrentWindowLifecycle() {
    // 两个 worker、无客户端（接受即超时）：窗口到期全部 join 回收，返回空汇总。
    std::vector<std::shared_ptr<FakeIpcServerBackend>> fakes;
    for (int i = 0; i < 2; ++i) {
        auto fake = std::make_shared<FakeIpcServerBackend>();
        fake->failAccept = true;
        fake->acceptErrorCode = ERROR_TIMEOUT;
        fakes.push_back(fake);
    }
    std::atomic<std::size_t> next{0};
    auto factory = [&]() -> std::shared_ptr<IpcSession> {
        const std::size_t i = next.fetch_add(1);
        return std::make_shared<IpcSession>(fakes.at(i));
    };
    const auto start = std::chrono::steady_clock::now();
    auto result = RunConcurrentServer(
        2, std::chrono::milliseconds(60), std::chrono::milliseconds(200),
        nullptr, factory);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!result || result.Value().workers != 2 ||
        result.Value().clientsServed != 0 ||
        result.Value().failedSessions != 0) {
        return false;
    }
    // 调用返回即 join 完成；窗口有界（容忍调度抖动），线程无脱逸。
    if (elapsed.count() < 40 || elapsed.count() > 5000) {
        return false;
    }
    for (const auto& fake : fakes) {
        if (fake->closeCount < 1) {
            return false;
        }
    }
    return true;
}

bool TestRunConcurrentServesClientSession() {
    // 单个 worker 成功服务一个客户端会话（首帧后帧间空闲正常结束），随后窗口到期。
    auto fake = std::make_shared<FakeIpcServerBackend>();
    fake->incoming = BuildFrame(IpcMessageType::Ping, 7, {});
    fake->failReadAtCall = 2; // 服务完首帧后读下一帧头超时
    fake->failReadAtErrorCode = ERROR_TIMEOUT;
    std::atomic<std::size_t> next{0};
    auto factory = [&]() -> std::shared_ptr<IpcSession> {
        (void)next.fetch_add(1);
        return std::make_shared<IpcSession>(fake);
    };
    auto result = RunConcurrentServer(
        1, std::chrono::milliseconds(80), std::chrono::milliseconds(200),
        nullptr, factory);
    if (!result) {
        return false;
    }
    const auto& summary = result.Value();
    if (summary.workers != 1 || summary.clientsServed != 1 ||
        summary.sessions.size() != 1) {
        return false;
    }
    const auto& served = summary.sessions[0];
    return served.requestType == IpcMessageType::Ping &&
           served.replyType == IpcMessageType::Ack &&
           served.framesServed == 1 && served.requestId == 7;
}

bool TestConcurrentInstancesServeTwoClients() {
    // 集成：两个真实管道实例并发监听同一管道名（PIPE_UNLIMITED_INSTANCES），
    // 两个客户端分别连接并被各自实例服务（互不阻塞）。
    const std::wstring pipeName =
        L"\\\\.\\pipe\\CppOptimizerIpcTestConcurrent";
    bool firstOk = false;
    bool secondOk = false;
    std::thread firstServer([&]() {
        IpcSession::Options options;
        options.clientGate = IpcSession::AllowAllClientGate; // 测试/隔离：显式放行
        options.ioTimeout = std::chrono::milliseconds(8000);
        IpcSession session(optimizer::ipc::CreateWin32ServerBackend(pipeName),
                           options);
        auto served =
            session.ServeOne(nullptr, std::chrono::milliseconds(8000));
        firstOk = served &&
                  served.Value().replyType == IpcMessageType::Ack;
    });
    std::thread secondServer([&]() {
        IpcSession::Options options;
        options.clientGate = IpcSession::AllowAllClientGate;
        options.ioTimeout = std::chrono::milliseconds(8000);
        IpcSession session(optimizer::ipc::CreateWin32ServerBackend(pipeName),
                           options);
        auto served =
            session.ServeOne(nullptr, std::chrono::milliseconds(8000));
        secondOk = served &&
                   served.Value().replyType == IpcMessageType::Ack;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 等待两实例就绪
    const auto runClient = [&pipeName](std::uint32_t id) -> bool {
        auto backend = optimizer::ipc::CreateWin32ClientBackend();
        auto reply =
            IpcRoundTrip(backend, pipeName, IpcMessageType::Ping, {}, id,
                         std::chrono::milliseconds(8000));
        return reply && reply.Value().type == IpcMessageType::Ack;
    };
    const bool clientA = runClient(1);
    const bool clientB = runClient(2);
    firstServer.join();
    secondServer.join();
    return clientA && clientB && firstOk && secondOk;
}

// ---------- 周期上报客户端（IPC-011，RunPeriodicReporter） ----------

// 构造一个“递增 requestId 的 Ping 帧”工厂（与运行器解耦的纯调用方）。
std::function<Result<IpcFrameRequest>()> PingRequestFactory(
    std::uint32_t& nextId) {
    return [&nextId]() -> Result<IpcFrameRequest> {
        IpcFrameRequest request;
        request.type = IpcMessageType::Ping;
        request.requestId = ++nextId;
        return Result<IpcFrameRequest>::Success(std::move(request));
    };
}

bool TestPeriodicReporterHappyAcks() {
    // 宿主在线：窗口内多个周期全部收到配对应答（每周期独立连接）。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    for (int i = 1; i <= 12; ++i) {
        const auto ack = BuildFrame(IpcMessageType::Ack, i, {});
        fake->incoming.insert(fake->incoming.end(), ack.begin(), ack.end());
    }
    std::uint32_t nextId = 0;
    IpcPeriodicReportOptions options;
    options.pipePath = L"\\.\\pipe\\test";
    options.window = std::chrono::milliseconds(150);
    options.interval = std::chrono::milliseconds(15);
    options.ioTimeout = std::chrono::milliseconds(100);
    options.maxConnectAttempts = 2;
    options.reconnectBackoff = std::chrono::milliseconds(1);
    const auto result = RunPeriodicReporter(
        fake, options, PingRequestFactory(nextId));
    if (!result || result.Value().reportsSent < 1 ||
        result.Value().connectFailures != 0 ||
        !result.Value().lastReplyAck) {
        return false;
    }
    return fake->closed; // 窗口结束关闭连接
}

bool TestPeriodicReporterHostOffline() {
    // 宿主不可达（连接失败）：可恢复失败计数、0 上报，窗口到期正常返回。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->failConnect = true;
    fake->failCode = ERROR_FILE_NOT_FOUND;
    std::uint32_t nextId = 0;
    IpcPeriodicReportOptions options;
    options.pipePath = L"\\.\\pipe\\test";
    options.window = std::chrono::milliseconds(120);
    options.interval = std::chrono::milliseconds(15);
    options.ioTimeout = std::chrono::milliseconds(100);
    options.maxConnectAttempts = 2;
    options.reconnectBackoff = std::chrono::milliseconds(1);
    const auto result = RunPeriodicReporter(
        fake, options, PingRequestFactory(nextId));
    return result && result.Value().reportsSent == 0 &&
           result.Value().connectFailures >= 1;
}

bool TestPeriodicReporterSampleFailureIsFatal() {
    // 采样失败（requestFactory 失败）为致命：原样上报并停止，不发起连接。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    std::uint32_t calls = 0;
    const auto badFactory = [&calls]() -> Result<IpcFrameRequest> {
        ++calls;
        return Result<IpcFrameRequest>::Failure(
            Error::Validation("agent", L"采样失败"));
    };
    IpcPeriodicReportOptions options;
    options.pipePath = L"\\.\\pipe\\test";
    options.window = std::chrono::milliseconds(5000);
    options.interval = std::chrono::milliseconds(1000);
    const auto result = RunPeriodicReporter(fake, options, badFactory);
    return !result && result.ErrorValue().domain == ErrorDomain::Validation &&
           calls == 1 && fake->connectCount == 0;
}

bool TestPeriodicReporterBoundedRetryRecovers() {
    // 单次上报的前 N 次连接失败后按上限重试成功：0 connect failures、有上报。
    auto fake = std::make_shared<FakeIpcClientBackend>();
    fake->failConnectCountdown = 2; // 头两次连接失败（宿主暂不可达），随后恢复
    for (int i = 1; i <= 20; ++i) {
        const auto ack = BuildFrame(IpcMessageType::Ack, i, {});
        fake->incoming.insert(fake->incoming.end(), ack.begin(), ack.end());
    }
    std::uint32_t nextId = 0;
    IpcPeriodicReportOptions options;
    options.pipePath = L"\\.\\pipe\\test";
    options.window = std::chrono::milliseconds(150);
    options.interval = std::chrono::milliseconds(15);
    options.ioTimeout = std::chrono::milliseconds(100);
    options.maxConnectAttempts = 3;
    options.reconnectBackoff = std::chrono::milliseconds(1);
    const auto result = RunPeriodicReporter(
        fake, options, PingRequestFactory(nextId));
    if (!result || result.Value().reportsSent < 1 ||
        result.Value().connectFailures != 0) {
        return false;
    }
    // 有界重试：发生过多次连接尝试（首周期两次失败 + 第三次成功）。
    return fake->connectCount >= 3;
}

// ---------- 自适应上报节奏（IPC-012，IpcReportGapAfterFailures） ----------

bool TestReportGapPureBasics() {
    using namespace std::chrono;
    // 0 次失败 = 基础间隔；失败指数放大；达 cap 封顶。
    const auto g0 =
        IpcReportGapAfterFailures(0, milliseconds(1000), milliseconds(5000));
    const auto g1 =
        IpcReportGapAfterFailures(1, milliseconds(1000), milliseconds(5000));
    const auto g2 =
        IpcReportGapAfterFailures(2, milliseconds(1000), milliseconds(5000));
    const auto g3 =
        IpcReportGapAfterFailures(3, milliseconds(1000), milliseconds(5000));
    const auto gMany =
        IpcReportGapAfterFailures(99, milliseconds(1000), milliseconds(5000));
    if (g0 != milliseconds(1000) || g1 != milliseconds(2000) ||
        g2 != milliseconds(4000) || g3 != milliseconds(5000) ||
        gMany != milliseconds(5000)) {
        return false;
    }
    // base 超过 cap：一律以 cap 为界。
    const auto clamp0 =
        IpcReportGapAfterFailures(0, milliseconds(1000), milliseconds(300));
    const auto clamp1 =
        IpcReportGapAfterFailures(1, milliseconds(1000), milliseconds(300));
    return clamp0 == milliseconds(300) && clamp1 == milliseconds(300);
}

bool TestReportGapPureOverflowSafe() {
    using namespace std::chrono;
    // 巨大 cap/大量失败：指数放大封顶且不发生乘法溢出。
    const auto huge = milliseconds(1LL << 62);
    const auto g =
        IpcReportGapAfterFailures(200, milliseconds(1000), huge);
    return g == huge && g.count() > 0;
}

bool TestReportGapPureDefensiveCapZero() {
    using namespace std::chrono;
    // cap<=0（未配置）：退化为固定基础间隔（防御分支）。
    const auto g = IpcReportGapAfterFailures(5, milliseconds(1000),
                                             milliseconds(0));
    return g == milliseconds(1000);
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

    run(L"make header fields", &TestMakeHeaderFields);
    run(L"message type names", &TestMessageTypeNames);
    run(L"parse message type strict", &TestParseMessageTypeStrict);
    run(L"validate header accepts valid", &TestValidateHeaderAcceptsValid);
    run(L"validate header rejects bad magic", &TestValidateHeaderRejectsBadMagic);
    run(L"validate header rejects bad version", &TestValidateHeaderRejectsBadVersion);
    run(L"validate header rejects unknown type", &TestValidateHeaderRejectsUnknownType);
    run(L"validate header rejects nonzero reserved",
        &TestValidateHeaderRejectsNonzeroReserved);
    run(L"validate header rejects oversized payload",
        &TestValidateHeaderRejectsOversizedPayload);
    run(L"serialize/parse header round trip", &TestSerializeParseRoundTrip);
    run(L"parse header rejects wrong size", &TestParseHeaderRejectsWrongSize);
    run(L"parse header rejects corrupt bytes", &TestParseHeaderRejectsCorruptBytes);
    run(L"facts serialize/parse round trip", &TestFactsSerializeParseRoundTrip);
    run(L"facts unicode value round trip", &TestFactsUnicodeValueRoundTrip);
    run(L"facts envelope only is empty facts", &TestFactsEnvelopeOnlyIsEmptyFacts);
    run(L"facts rejects empty payload", &TestFactsRejectsEmptyPayload);
    run(L"facts rejects missing or wrong envelope",
        &TestFactsRejectsMissingOrWrongEnvelope);
    run(L"facts rejects trailing newline or blank line",
        &TestFactsRejectsTrailingNewlineOrBlankLine);
    run(L"facts rejects carriage return", &TestFactsRejectsCarriageReturn);
    run(L"facts rejects missing equals", &TestFactsRejectsMissingEquals);
    run(L"facts rejects empty key", &TestFactsRejectsEmptyKey);
    run(L"facts rejects illegal key characters",
        &TestFactsRejectsIllegalKeyCharacters);
    run(L"facts rejects duplicate key", &TestFactsRejectsDuplicateKey);
    run(L"facts rejects control bytes", &TestFactsRejectsControlBytes);
    run(L"facts rejects oversized key or value",
        &TestFactsRejectsOversizedKeyOrValue);
    run(L"facts rejects too many entries", &TestFactsRejectsTooManyEntries);
    run(L"facts serialize rejects invalid input",
        &TestFactsSerializeRejectsInvalidInput);
    run(L"facts summary empty and basic", &TestFactsSummaryEmptyAndBasic);
    run(L"facts summary truncation bounded", &TestFactsSummaryTruncationBounded);
    run(L"facts schema accepts valid sets", &TestFactsSchemaAcceptsValidSets);
    run(L"facts schema rejects empty", &TestFactsSchemaRejectsEmpty);
    run(L"facts schema rejects unknown key", &TestFactsSchemaRejectsUnknownKey);
    run(L"facts schema rejects non numeric value",
        &TestFactsSchemaRejectsNonNumericValue);
    run(L"facts schema rejects out of range", &TestFactsSchemaRejectsOutOfRange);
    run(L"facts schema rejects available gt total",
        &TestFactsSchemaRejectsAvailableGtTotal);
    run(L"facts schema rejects empty observer", &TestFactsSchemaRejectsEmptyObserver);
    run(L"facts schema token rules", &TestFactsSchemaTokenRules);
    run(L"facts token excluded from summary", &TestFactsTokenExcludedFromSummary);
    run(L"credentials provision read round trip",
        &TestCredentialsProvisionReadRoundTrip);
    run(L"credentials provision idempotent", &TestCredentialsProvisionIdempotent);
    run(L"credentials force rotation changes token",
        &TestCredentialsForceRotationChangesToken);
    run(L"credentials rejects invalid content", &TestCredentialsRejectsInvalidContent);
    run(L"credentials dacl restricted to system and user",
        &TestCredentialsDaclRestrictedToSystemAndUser);
    run(L"serve one ping -> ack", &TestServeOnePingAck);
    run(L"serve one facts snapshot -> ack", &TestServeOneFactsSnapshotAck);
    run(L"serve one invalid facts -> error reply", &TestServeOneInvalidFactsReply);
    run(L"serve one unknown facts key -> error reply",
        &TestServeOneUnknownFactsKeyReply);
    run(L"serve one empty facts -> error reply", &TestServeOneEmptyFactsReply);
    run(L"session gate rejects session zero", &TestSessionGateRejectsSessionZero);
    run(L"session gate rejects unknown pid", &TestSessionGateRejectsUnknownPid);
    run(L"session gate custom allow override", &TestSessionGateCustomAllowOverride);
    run(L"session gate custom reject", &TestSessionGateCustomReject);
    run(L"serve token matching -> ack", &TestServeTokenMatchingAck);
    run(L"serve token missing -> auth failed", &TestServeTokenMissingRejected);
    run(L"serve token mismatch -> auth failed", &TestServeTokenMismatchRejected);
    run(L"serve token ignored when not configured",
        &TestServeTokenIgnoredWhenNotConfigured);
    run(L"session sid gate allows matching user",
        &TestSessionSidGateAllowsMatchingUser);
    run(L"session sid gate rejects unknown sid", &TestSessionSidGateRejectsUnknownSid);
    run(L"session sid gate rejects mismatch", &TestSessionSidGateRejectsMismatch);
    run(L"session sid gate disabled when empty", &TestSessionSidGateDisabledWhenEmpty);
    run(L"serve persistent reuses instance across clients",
        &TestServePersistentReusesInstanceAcrossClients);
    run(L"serve persistent accept timeout keeps instance",
        &TestServePersistentAcceptTimeoutKeepsInstance);
    run(L"serve non persistent accept timeout closes",
        &TestServeNonPersistentAcceptTimeoutCloses);
    run(L"serve session two pings one connection",
        &TestServeSessionTwoPingsOneConnection);
    run(L"serve session facts two frames", &TestServeSessionFactsTwoFrames);
    run(L"serve session idle timeout ends", &TestServeSessionIdleTimeoutEnds);
    run(L"serve session budget ends", &TestServeSessionBudgetEnds);
    run(L"serve session invalid second frame ends",
        &TestServeSessionInvalidSecondFrameEnds);
    run(L"serve session token checked every frame",
        &TestServeSessionTokenCheckedEveryFrame);
    run(L"serve session connect then close is failure",
        &TestServeSessionConnectThenCloseIsFailure);
    run(L"serve session zero params rejected",
        &TestServeSessionZeroParamsRejected);
    run(L"verdict observer accepted on serve",
        &TestVerdictObserverAcceptedOnServe);
    run(L"verdict observer gate reject", &TestVerdictObserverGateReject);
    run(L"verdict observer sid reject", &TestVerdictObserverSidReject);
    run(L"verdict observer auth failed", &TestVerdictObserverAuthFailed);
    run(L"serve one custom handler", &TestServeOneCustomHandler);
    run(L"serve one bad magic rejected", &TestServeOneBadMagicRejected);
    run(L"serve one unknown version rejected", &TestServeOneUnknownVersionRejected);
    run(L"serve one unknown type rejected", &TestServeOneUnknownTypeRejected);
    run(L"serve one oversized payload rejected", &TestServeOneOversizedPayloadRejected);
    run(L"serve one accept timeout", &TestServeOneAcceptTimeout);
    run(L"serve one create failure", &TestServeOneCreateFailure);
    run(L"serve one read failure not disguised",
        &TestServeOneReadFailureNotDisguised);
    run(L"serve one write failure not disguised",
        &TestServeOneWriteFailureNotDisguised);
    run(L"serve one handler failure sends error",
        &TestServeOneHandlerFailureSendsError);
    run(L"serve one default handler rejects request type",
        &TestServeOneDefaultHandlerRejectsRequestType);
    run(L"round trip ping -> ack", &TestRoundTripPingAck);
    run(L"round trip facts payload", &TestRoundTripFactsPayload);
    run(L"round trip error reply not disguised",
        &TestRoundTripErrorReplyNotDisguised);
    run(L"round trip unauthorized error reply not disguised",
        &TestRoundTripUnauthorizedErrorReplyNotDisguised);
    run(L"round trip auth failed error reply not disguised",
        &TestRoundTripAuthFailedReplyNotDisguised);
    run(L"round trip request id mismatch", &TestRoundTripRequestIdMismatch);
    run(L"round trip connect failure", &TestRoundTripConnectFailure);
    run(L"round trip write failure", &TestRoundTripWriteFailure);
    run(L"round trip read failure", &TestRoundTripReadFailure);
    run(L"round trip session two pings", &TestRoundTripSessionTwoPings);
    run(L"round trip session empty no connect",
        &TestRoundTripSessionEmptyNoConnect);
    run(L"round trip session second error aborts",
        &TestRoundTripSessionSecondErrorAborts);
    run(L"round trip session mid request id mismatch",
        &TestRoundTripSessionMidRequestIdMismatch);
    run(L"round trip session connect failure", &TestRoundTripSessionConnectFailure);
    run(L"run concurrent rejects bad params", &TestRunConcurrentRejectsBadParams);
    run(L"run concurrent window lifecycle", &TestRunConcurrentWindowLifecycle);
    run(L"run concurrent serves client session",
        &TestRunConcurrentServesClientSession);
    run(L"concurrent instances serve two clients",
        &TestConcurrentInstancesServeTwoClients);
    run(L"periodic reporter happy acks", &TestPeriodicReporterHappyAcks);
    run(L"periodic reporter host offline", &TestPeriodicReporterHostOffline);
    run(L"periodic reporter sample failure is fatal",
        &TestPeriodicReporterSampleFailureIsFatal);
    run(L"periodic reporter bounded retry recovers",
        &TestPeriodicReporterBoundedRetryRecovers);
    run(L"report gap pure basics", &TestReportGapPureBasics);
    run(L"report gap pure overflow safe", &TestReportGapPureOverflowSafe);
    run(L"report gap pure defensive cap zero", &TestReportGapPureDefensiveCapZero);
    return failed == 0 ? 0 : 1;
}
