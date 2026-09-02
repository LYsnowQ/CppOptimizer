#include "ipc/ipc_session.hpp"

#include "ipc/ipc_facts.hpp"

#include <array>
#include <cwctype>
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

// agent_token 事实值与期望凭据逐字节比较。期望凭据仅支持 ASCII（<=0x7F），
// 含非 ASCII 一律视为不匹配（CLI/供给层已约束 ASCII 字母数字/_/-）。
bool TokenMatches(std::string_view token,
                  std::wstring_view expected) noexcept {
    if (token.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < token.size(); ++i) {
        const wchar_t e = expected[i];
        if (e > 0x7F) {
            return false;
        }
        if (static_cast<unsigned char>(token[i]) != e) {
            return false;
        }
    }
    return true;
}

// SID 字符串大小写不敏感比较（SID 十六进制段允许大小写混写）。
bool SidEqualsIgnoreCase(std::wstring_view a,
                         std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
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
        case IpcErrorCode::AuthFailed:
            return L"authentication failed";
    }
    return L"unknown error";
}

IpcSession::IpcSession(std::shared_ptr<IpcServerBackend> backend,
                       Options options)
    : backend_(std::move(backend)), options_(options) {}

void IpcSession::Close() noexcept {
    backend_->Close();
}

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
    // 统一收尾：错误应答路径先“排空读”等待对端读完 Error 帧（防 Disconnect
    // 竞态——对端读到 233 断管而非错误码），再断开；最后按 persistentAccept 决定
    // 是否保留监听实例（SVC-003 连续受理：实例在 ServeOne 调用间隙保持监听，
    // 消除“每轮重建+空窗”问题）。对端已关/超时均视为尽力而为，不阻断主错误上报。
    const auto drainAndDisconnect = [&backend, this]() {
        std::array<std::byte, 1> drain{};
        (void)backend.ReadAll(drain, options_.ioTimeout);
        (void)backend.DisconnectClient();
    };
    const auto closeUnlessPersistent = [&backend, this]() {
        if (!options_.persistentAccept) {
            backend.Close();
        }
    };

    if (auto created = backend.CreateAndListen(); !created) {
        return common::Result<IpcServeResult>::Failure(created.ErrorValue());
    }
    if (auto accepted = backend.AcceptClient(acceptTimeout); !accepted) {
        // 持续模式接受超时：保留监听实例返回 ERROR_TIMEOUT，由调用方决定继续/结束。
        closeUnlessPersistent();
        return common::Result<IpcServeResult>::Failure(accepted.ErrorValue());
    }

    // 读取并解析帧头（严格校验；非法帧 -> Error 应答并断开）。
    std::array<std::byte, kIpcHeaderSize> headerBytes{};
    if (auto readHeader = backend.ReadAll(headerBytes, options_.ioTimeout);
        !readHeader) {
        // 客户端连上即断开/读失败：断开本次连接；持续模式保留监听实例。
        (void)backend.DisconnectClient();
        closeUnlessPersistent();
        return common::Result<IpcServeResult>::Failure(
            readHeader.ErrorValue());
    }
    auto parsedHeader = ParseHeader(headerBytes);
    if (!parsedHeader) {
        SendErrorReply(backend, 0, IpcErrorCode::InvalidHeader,
                       options_.ioTimeout);
        drainAndDisconnect();
        closeUnlessPersistent();
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
            closeUnlessPersistent();
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
    const IpcClientIdentity identity{request.clientPid, request.clientSessionId,
                                     backend.ClientUserSid()};
    if (options_.clientGate) {
        if (auto gated = options_.clientGate(identity); !gated) {
            SendErrorReply(backend, request.requestId,
                           IpcErrorCode::UnauthorizedClient,
                           options_.ioTimeout);
            drainAndDisconnect();
            closeUnlessPersistent();
            return common::Result<IpcServeResult>::Failure(
                gated.ErrorValue());
        }
    }
    // SID 授权白名单（IPC-006）：配置后要求客户端用户 SID 命中其一（大小写不敏感）。
    if (!options_.allowedClientSids.empty()) {
        bool sidAllowed = false;
        for (const std::wstring& sid : options_.allowedClientSids) {
            if (SidEqualsIgnoreCase(identity.userSid, sid)) {
                sidAllowed = true;
                break;
            }
        }
        if (!sidAllowed) {
            SendErrorReply(backend, request.requestId,
                           IpcErrorCode::UnauthorizedClient,
                           options_.ioTimeout);
            drainAndDisconnect();
            closeUnlessPersistent();
            return common::Result<IpcServeResult>::Failure(
                common::Error::Validation(
                    "SessionSidGate", L"客户端用户不在授权白名单"));
        }
    }

    IpcReply reply;
    // 无自定义处理器时使用默认处理器（可携带会话凭据要求 expectedToken）。
    const bool customHandler = static_cast<bool>(handler);
    const common::Result<void> handled =
        customHandler
            ? handler(request, reply)
            : DefaultHandler(request, reply, options_.expectedToken);
    if (!handled) {
        SendErrorReply(backend, request.requestId,
                       handler ? IpcErrorCode::HandlerFailed
                               : IpcErrorCode::UnsupportedType,
                       options_.ioTimeout);
        drainAndDisconnect();
        closeUnlessPersistent();
        return common::Result<IpcServeResult>::Failure(
            handled.ErrorValue());
    }

    // 校验应答帧（载荷超限属内部错误，不写出、不伪装成功）。
    if (const auto valid = ValidateIpcHeader(MakeIpcHeader(
            reply.type, static_cast<std::uint32_t>(reply.payload.size()),
            request.requestId));
        !valid) {
        (void)backend.DisconnectClient();
        closeUnlessPersistent();
        return common::Result<IpcServeResult>::Failure(valid.ErrorValue());
    }

    if (auto written = backend.WriteAll(
            MakeFrame(reply.type, request.requestId, reply.payload),
            options_.ioTimeout);
        !written) {
        (void)backend.DisconnectClient();
        closeUnlessPersistent();
        return common::Result<IpcServeResult>::Failure(written.ErrorValue());
    }

    // 等待客户端关闭（经典命名管道握手：断开前先确认对端已读完应答，
    // 避免 DisconnectNamedPipe 竞态导致客户端读应答失败）。
    // 0 字节/对端关闭/超时均视为可安全断开（尽力而为，不阻断结果）。
    drainAndDisconnect();
    closeUnlessPersistent();

    IpcServeResult result;
    result.requestType = request.type;
    result.replyType = reply.type;
    result.requestId = request.requestId;
    result.clientPid = request.clientPid;
    result.clientSessionId = request.clientSessionId;
    result.clientUserSid = identity.userSid;
    result.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
    return common::Result<IpcServeResult>::Success(std::move(result));
}

common::Result<void> IpcSession::DefaultHandler(const IpcRequest& request,
                                                IpcReply& reply) {
    // 两参版本＝不带会话凭据要求（等价 IPC-002/003 行为）。
    return DefaultHandler(request, reply, std::wstring_view{});
}

common::Result<void> IpcSession::DefaultHandler(const IpcRequest& request,
                                                IpcReply& reply,
                                                std::wstring_view expectedToken) {
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
            // 会话凭据（IPC-005）：服务端配置 expectedToken 时，载荷必须携带完全
            // 匹配的 agent_token 事实，否则回 Error(AuthFailed)（不伪装成功）。
            if (!expectedToken.empty()) {
                bool matched = false;
                for (const IpcFact& fact : parsed.Value()) {
                    if (fact.key == kFactsTokenKey &&
                        TokenMatches(fact.value, expectedToken)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    reply.type = IpcMessageType::Error;
                    reply.payload.clear();
                    reply.payload.push_back(
                        static_cast<std::byte>(IpcErrorCode::AuthFailed));
                    break;
                }
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
