#include "ipc/ipc_protocol.hpp"
#include "ipc/ipc_session.hpp"
#include "ipc/ipc_transport.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::ErrorDomain;
using optimizer::common::Result;
using optimizer::ipc::IpcClientBackend;
using optimizer::ipc::IpcErrorCode;
using optimizer::ipc::IpcHeader;
using optimizer::ipc::IpcMessageType;
using optimizer::ipc::IpcReply;
using optimizer::ipc::IpcRequest;
using optimizer::ipc::IpcRoundTrip;
using optimizer::ipc::IpcServeResult;
using optimizer::ipc::IpcServerBackend;
using optimizer::ipc::IpcSession;
using optimizer::ipc::kIpcHeaderSize;
using optimizer::ipc::kIpcMagic;
using optimizer::ipc::kIpcVersion;
using optimizer::ipc::kMaxPayloadLength;
using optimizer::ipc::MakeIpcHeader;
using optimizer::ipc::MessageTypeToString;
using optimizer::ipc::ParseHeader;
using optimizer::ipc::ParseMessageType;
using optimizer::ipc::SerializeHeader;
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

    std::vector<std::byte> incoming; // 模拟客户端注入的完整帧
    std::size_t readOffset = 0;
    std::vector<std::byte> outgoing; // 记录服务端写出的帧
    int createCount = 0;
    int acceptCount = 0;
    int disconnectCount = 0;
    int closeCount = 0;

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
};

// ---------- fake 客户端后端 ----------

class FakeIpcClientBackend final : public IpcClientBackend {
public:
    bool failConnect = false;
    bool failWrite = false;
    bool failRead = false;
    std::uint32_t failCode = ERROR_ACCESS_DENIED;

    std::wstring connectedPath;
    std::vector<std::byte> incoming; // 应答帧
    std::size_t readOffset = 0;
    std::vector<std::byte> outgoing; // 请求帧
    bool closed = false;

    Result<void> Connect(std::wstring_view pipePath,
                         std::chrono::milliseconds) override {
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
    const auto payload = BytesFromText("hello");
    fake->incoming = BuildFrame(IpcMessageType::FactsSnapshot, 3, payload);
    IpcSession session(fake);
    auto served = session.ServeOne(nullptr, std::chrono::milliseconds(100));
    if (!served || served.Value().payloadBytes != 5) {
        return false;
    }
    IpcHeader header;
    std::vector<std::byte> replyPayload;
    return ParseWrittenFrame(fake->outgoing, header, replyPayload) &&
           header.type == static_cast<std::uint8_t>(IpcMessageType::Ack) &&
           header.requestId == 3 && !replyPayload.empty();
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

// ---------- 客户端往返（fake 后端） ----------

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
    run(L"serve one ping -> ack", &TestServeOnePingAck);
    run(L"serve one facts snapshot -> ack", &TestServeOneFactsSnapshotAck);
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
    run(L"round trip request id mismatch", &TestRoundTripRequestIdMismatch);
    run(L"round trip connect failure", &TestRoundTripConnectFailure);
    run(L"round trip write failure", &TestRoundTripWriteFailure);
    run(L"round trip read failure", &TestRoundTripReadFailure);
    return failed == 0 ? 0 : 1;
}
