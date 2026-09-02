#include "ipc/ipc_session.hpp"

#include "ipc/ipc_facts.hpp"

#include <array>
#include <string>

namespace optimizer::ipc {

namespace {

// 组装一帧（帧头 + 载荷）。
std::vector<std::byte> MakeFrame(IpcMessageType type, std::uint32_t requestId,
                                 std::span<const std::byte> payload) noexcept {
    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    SerializeHeader(MakeIpcHeader(type,
                                  static_cast<std::uint32_t>(payload.size()),
                                  requestId),
                    headerBytes);
    std::vector<std::byte> frame;
    frame.reserve(kIpcHeaderSize + payload.size());
    frame.insert(frame.end(), headerBytes.begin(), headerBytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// 发送 Error 应答（尽力而为：失败不阻断主错误上报）。
void SendErrorReply(IpcServerBackend& backend, std::uint32_t requestId,
                    IpcErrorCode code,
                    std::chrono::milliseconds timeout) noexcept {
    const std::vector<std::byte> payload{
        static_cast<std::byte>(code)};
    (void)backend.WriteAll(MakeFrame(IpcMessageType::Error, requestId, payload),
                           timeout);
}

// UTF-8 文本 -> 字节载荷。
std::vector<std::byte> BytesFromText(std::string_view text) noexcept {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char ch : text) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return bytes;
}

} // namespace

const wchar_t* IpcErrorCodeToString(IpcErrorCode code) noexcept {
    switch (code) {
        case IpcErrorCode::InvalidHeader:
            return L"invalid header";
        case IpcErrorCode::UnsupportedType:
            return L"unsupported message type";
        case IpcErrorCode::HandlerFailed:
            return L"handler failed";
        case IpcErrorCode::Internal:
            return L"internal error";
        case IpcErrorCode::InvalidFacts:
            return L"invalid facts payload";
        case IpcErrorCode::UnauthorizedClient:
            return L"unauthorized client";
    }
    return L"unknown error";
}

IpcSession::IpcSession(std::shared_ptr<IpcServerBackend> backend,
                       Options options)
    : backend_(std::move(backend)), options_(options) {}

common::Result<void> IpcSession::DefaultClientGate(
    const IpcClientIdentity& client) noexcept {
    if (client.pid == 0) {
        return common::Result<void>::Failure(common::Error::Validation(
            "DefaultClientGate", L"客户端 PID 不可识别（0），拒绝受理"));
    }
    if (client.sessionId == 0) {
        return common::Result<void>::Failure(common::Error::Validation(
            "DefaultClientGate", L"客户端位于会话 0（非交互），拒绝受理"));
    }
    return common::Result<void>::Success();
}

common::Result<void> IpcSession::AllowAllClientGate(
    const IpcClientIdentity&) noexcept {
    return common::Result<void>::Success();
}

common::Result<IpcServeResult> IpcSession::ServeOne(
    Handler handler, std::chrono::milliseconds acceptTimeout) {
    auto& backend = *backend_;

    if (auto created = backend.CreateAndListen(); !created) {
        return common::Result<IpcServeResult>::Failure(created.ErrorValue());
    }
    if (auto accepted = backend.AcceptClient(acceptTimeout); !accepted) {
        backend.Close();
        return common::Result<IpcServeResult>::Failure(accepted.ErrorValue());
    }

    // 读取并解析帧头（严格校验；非法帧 -> Error 应答并断开）。
    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    if (auto readHeader = backend.ReadAll(headerBytes, options_.ioTimeout);
        !readHeader) {
        backend.Close();
        return common::Result<IpcServeResult>::Failure(
            readHeader.ErrorValue());
    }
    auto parsedHeader = ParseHeader(headerBytes);
    if (!parsedHeader) {
        SendErrorReply(backend, 0, IpcErrorCode::InvalidHeader,
                       options_.ioTimeout);
(void)backend.DisconnectClient();
        backend.Close();
        return common::Result<IpcServeResult>::Failure(
            parsedHeader.ErrorValue());
    }
    const IpcHeader header = parsedHeader.Value();

    // 读取载荷。
    std::vector<std::byte> payload;
    if (header.payloadLength > 0) {
        payload.resize(header.payloadLength);
        if (auto readPayload = backend.ReadAll(payload, options_.ioTimeout);
            !readPayload) {
            (void)backend.DisconnectClient();
            backend.Close();
            return common::Result<IpcServeResult>::Failure(
                readPayload.ErrorValue());
        }
    }

    // 组装请求（帧头已校验，类型必然可解析）。
    IpcRequest request;
    request.type = ParseMessageType(header.type).value();
    request.requestId = header.requestId;
    request.payload = std::move(payload);
    request.clientPid = backend.ClientPid();
    request.clientSessionId = backend.ClientSessionId();

    // 会话级身份裁决：受理前按客户端身份决定是否放行。拒绝 -> Error 应答并断开。
    // Options 缺省启用 DefaultClientGate；置空 clientGate 视为放行（防误用）。
    const IpcClientIdentity identity{request.clientPid,
                                     request.clientSessionId};
    if (options_.clientGate) {
        if (auto gated = options_.clientGate(identity); !gated) {
            SendErrorReply(backend, request.requestId,
                           IpcErrorCode::UnauthorizedClient,
                           options_.ioTimeout);
            (void)backend.DisconnectClient();
            backend.Close();
            return common::Result<IpcServeResult>::Failure(
                gated.ErrorValue());
        }
    }

    IpcReply reply;
    const common::Result<void> handled =
        handler ? handler(request, reply) : DefaultHandler(request, reply);
    if (!handled) {
        SendErrorReply(backend, request.requestId,
                       handler ? IpcErrorCode::HandlerFailed
                               : IpcErrorCode::UnsupportedType,
                       options_.ioTimeout);
(void)backend.DisconnectClient();
        backend.Close();
        return common::Result<IpcServeResult>::Failure(
            handled.ErrorValue());
    }

    // 校验应答帧（载荷超限属内部错误，不写出、不伪装成功）。
    if (const auto valid = ValidateIpcHeader(MakeIpcHeader(
            reply.type, static_cast<std::uint32_t>(reply.payload.size()),
            request.requestId));
        !valid) {
        (void)backend.DisconnectClient();
        backend.Close();
        return common::Result<IpcServeResult>::Failure(valid.ErrorValue());
    }

    if (auto written = backend.WriteAll(
            MakeFrame(reply.type, request.requestId, reply.payload),
            options_.ioTimeout);
        !written) {
        (void)backend.DisconnectClient();
        backend.Close();
        return common::Result<IpcServeResult>::Failure(written.ErrorValue());
    }

    // 等待客户端关闭（经典命名管道握手：断开前先确认对端已读完应答，
    // 避免 DisconnectNamedPipe 竞态导致客户端读应答失败）。
    // 0 字节/对端关闭/超时均视为可安全断开（尽力而为，不阻断结果）。
    std::array<std::byte, 1> drain{};
    (void)backend.ReadAll(drain, options_.ioTimeout);

    (void)backend.DisconnectClient();
    backend.Close();

    IpcServeResult result;
    result.requestType = request.type;
    result.replyType = reply.type;
    result.requestId = request.requestId;
    result.clientPid = request.clientPid;
    result.clientSessionId = request.clientSessionId;
    result.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
    return common::Result<IpcServeResult>::Success(std::move(result));
}

common::Result<void> IpcSession::DefaultHandler(const IpcRequest& request,
                                                IpcReply& reply) {
    switch (request.type) {
        case IpcMessageType::Ping:
            reply.type = IpcMessageType::Ack;
            break;
        case IpcMessageType::FactsSnapshot: {
            auto parsed = ParseFactsV1(request.payload);
            // 语法契约（Parse）与 v1 键语义白名单（schema）任一违反都整体拒绝。
            const bool valid =
                parsed &&
                static_cast<bool>(ValidateFactsV1Schema(parsed.Value()));
            if (!valid) {
                // 载荷违反 CPOPFACTS/1 契约：回 Error(InvalidFacts)（不宽松接受）。
                reply.type = IpcMessageType::Error;
                reply.payload.clear();
                reply.payload.push_back(
                    static_cast<std::byte>(IpcErrorCode::InvalidFacts));
                break;
            }
            reply.type = IpcMessageType::Ack;
            reply.payload = BytesFromText(FormatFactsSummary(parsed.Value()));
            break;
        }
        default:
            return common::Result<void>::Failure(common::Error::Validation(
                "DefaultHandler", L"不支持的消息类型"));
    }
    return common::Result<void>::Success();
}

common::Result<IpcReply> IpcRoundTrip(
    std::shared_ptr<IpcClientBackend> backend, std::wstring_view pipePath,
    IpcMessageType type, std::span<const std::byte> payload,
    std::uint32_t requestId, std::chrono::milliseconds timeout) {
    auto& client = *backend;

    if (auto connected = client.Connect(pipePath, timeout); !connected) {
        return common::Result<IpcReply>::Failure(connected.ErrorValue());
    }
    if (auto written = client.WriteAll(MakeFrame(type, requestId, payload),
                                       timeout);
        !written) {
        client.Close();
        return common::Result<IpcReply>::Failure(written.ErrorValue());
    }

    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    if (auto readHeader = client.ReadAll(headerBytes, timeout); !readHeader) {
        client.Close();
        return common::Result<IpcReply>::Failure(readHeader.ErrorValue());
    }
    auto parsedHeader = ParseHeader(headerBytes);
    if (!parsedHeader) {
        client.Close();
        return common::Result<IpcReply>::Failure(parsedHeader.ErrorValue());
    }
    const IpcHeader header = parsedHeader.Value();

    std::vector<std::byte> replyPayload;
    if (header.payloadLength > 0) {
        replyPayload.resize(header.payloadLength);
        if (auto readPayload = client.ReadAll(replyPayload, timeout);
            !readPayload) {
            client.Close();
            return common::Result<IpcReply>::Failure(readPayload.ErrorValue());
        }
    }
    client.Close();

    // 应答 requestId 必须与请求配对（防乱序/伪造）；不匹配拒绝。
    if (header.requestId != requestId) {
        return common::Result<IpcReply>::Failure(common::Error::Validation(
            "IpcRoundTrip", L"应答 requestId 与请求不匹配"));
    }
    const IpcMessageType replyType = ParseMessageType(header.type).value();
    if (replyType == IpcMessageType::Error) {
        // Error 应答：载荷首字节为错误码（空载荷按内部错误处理）。
        const IpcErrorCode code =
            replyPayload.empty()
                ? IpcErrorCode::Internal
                : static_cast<IpcErrorCode>(replyPayload[0]);
        return common::Result<IpcReply>::Failure(common::Error::Validation(
            "IpcRoundTrip",
            std::wstring(L"服务端返回错误: ") + IpcErrorCodeToString(code)));
    }

    IpcReply reply;
    reply.type = replyType;
    reply.payload = std::move(replyPayload);
    return common::Result<IpcReply>::Success(std::move(reply));
}

} // namespace optimizer::ipc
