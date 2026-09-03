#include "ipc/ipc_session.hpp"

#include "ipc/ipc_facts.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>

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

const wchar_t* IpcSessionEndReasonToString(
    IpcSessionEndReason reason) noexcept {
    switch (reason) {
        case IpcSessionEndReason::ClientClosed:
            return L"client closed";
        case IpcSessionEndReason::IdleTimeout:
            return L"idle timeout";
        case IpcSessionEndReason::SessionBudget:
            return L"session budget";
    }
    return L"unknown";
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

common::Result<IpcServeResult> IpcSession::ServeLoop(
    Handler handler, std::chrono::milliseconds acceptTimeout,
    std::chrono::milliseconds idleTimeout,
    std::chrono::milliseconds sessionBudget, std::uint32_t maxFrames) {
    auto& backend = *backend_;
    // 统一收尾：错误应答/会话结束路径先“排空读”等待对端读完（防 Disconnect 竞态
    // ——对端读到 233 断管而非错误码/应答），再断开；最后按 persistentAccept 决定
    // 是否保留监听实例（SVC-003 连续受理：实例在调用间隙保持监听，消除“每轮重建
    // +空窗”问题）。对端已关/超时均视为尽力而为，不阻断主错误上报。
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
    // 客户端受理结论回调（连接层面，至多一次）：宿主据此计数身份/凭据失败（Safe Mode）。
    const auto emitVerdict = [this](IpcClientVerdict verdict) {
        if (options_.verdictObserver) {
            options_.verdictObserver(verdict);
        }
    };
    // 单帧模式（ServeOne）不设空闲/预算语义：读帧恒用 ioTimeout。
    const bool limitedFrames = (maxFrames != 0);
    const auto sessionDeadline =
        std::chrono::steady_clock::now() + sessionBudget;

    if (auto created = backend.CreateAndListen(); !created) {
        return common::Result<IpcServeResult>::Failure(created.ErrorValue());
    }
    if (auto accepted = backend.AcceptClient(acceptTimeout); !accepted) {
        // 接受超时：持续模式保留监听实例返回 ERROR_TIMEOUT，由调用方决定继续/结束。
        closeUnlessPersistent();
        return common::Result<IpcServeResult>::Failure(accepted.ErrorValue());
    }

    std::uint32_t frames = 0;   // 本次连接已成功服务帧数
    IpcServeResult last;        // 最近一帧摘要
    IpcSessionEndReason endReason = IpcSessionEndReason::ClientClosed;
    bool clientAuthRejected = false; // 本次连接是否出现过凭据不符（AuthFailed 回执）

    for (;;) {
        // 会话预算检查（多帧模式）：预算耗尽且已服务过帧 -> 正常结束；尚未服务
        // 任何帧 -> 视为接受后无帧可服务（ERROR_TIMEOUT，不伪装成功）。
        std::chrono::milliseconds headerTimeout = options_.ioTimeout;
        if (!limitedFrames) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    sessionDeadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                if (frames == 0) {
                    (void)backend.DisconnectClient();
                    closeUnlessPersistent();
                    return common::Result<IpcServeResult>::Failure(
                        common::Error::FromWin32(ERROR_TIMEOUT, "ServeSession"));
                }
                endReason = IpcSessionEndReason::SessionBudget;
                break;
            }
            headerTimeout = std::min(idleTimeout, remaining);
        }

        // 读取并解析帧头（严格校验；非法帧 -> Error 应答并断开）。
        std::array<std::byte, kIpcHeaderSize> headerBytes{};
        if (auto readHeader = backend.ReadAll(headerBytes, headerTimeout);
            !readHeader) {
            const auto& error = readHeader.ErrorValue();
            const bool clientClosed =
                error.domain == common::ErrorDomain::Win32 &&
                (error.code == ERROR_BROKEN_PIPE ||
                 error.code == ERROR_NO_DATA);
            const bool timedOut =
                error.domain == common::ErrorDomain::Win32 &&
                error.code == ERROR_TIMEOUT;
            // 多帧模式：客户端在已服务多帧后关闭/心跳丢失 -> 会话正常结束。
            if (clientClosed && frames > 0) {
                endReason = IpcSessionEndReason::ClientClosed;
                break;
            }
            if (timedOut && frames > 0) {
                // 空闲超时与预算耗尽同源于读超时（超时值取 min(idle, remaining)）：
                // 等于 idleTimeout 说明 idle 先到（心跳丢失），否则预算先耗尽。
                endReason =
                    (headerTimeout == idleTimeout)
                        ? IpcSessionEndReason::IdleTimeout
                        : IpcSessionEndReason::SessionBudget;
                break;
            }
            // 尚未服务任何帧即关闭/超时/读失败：断开本次连接（同单帧语义）。
            (void)backend.DisconnectClient();
            closeUnlessPersistent();
            return common::Result<IpcServeResult>::Failure(error);
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

        // 会话级身份裁决 + SID 授权白名单：受理帧前按客户端身份决定是否放行
        // （同一连接内身份固定，逐帧执行结果不变；拒绝 -> Error 应答并断开）。
        // Options 缺省启用 DefaultClientGate；置空 clientGate 视为放行（防误用）。
        const IpcClientIdentity identity{request.clientPid,
                                         request.clientSessionId,
                                         backend.ClientUserSid()};
        if (options_.clientGate) {
            if (auto gated = options_.clientGate(identity); !gated) {
                SendErrorReply(backend, request.requestId,
                               IpcErrorCode::UnauthorizedClient,
                               options_.ioTimeout);
                emitVerdict(IpcClientVerdict::UnauthorizedClient);
                drainAndDisconnect();
                closeUnlessPersistent();
                return common::Result<IpcServeResult>::Failure(
                    gated.ErrorValue());
            }
        }
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
                emitVerdict(IpcClientVerdict::UnauthorizedClient);
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
        // 凭据不符（默认处理器回 Error(AuthFailed)，非处理器失败）：标记本次连接结论。
        if (reply.type == IpcMessageType::Error &&
            reply.payload.size() == 1 &&
            static_cast<IpcErrorCode>(reply.payload[0]) ==
                IpcErrorCode::AuthFailed) {
            clientAuthRejected = true;
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
            return common::Result<IpcServeResult>::Failure(
                written.ErrorValue());
        }

        ++frames;
        last.requestType = request.type;
        last.replyType = reply.type;
        last.requestId = request.requestId;
        last.clientPid = request.clientPid;
        last.clientSessionId = request.clientSessionId;
        last.clientUserSid = identity.userSid;
        last.payloadBytes =
            static_cast<std::uint32_t>(request.payload.size());

        // 单帧模式（ServeOne）：服务满 maxFrames 帧即正常结束。
        if (limitedFrames && frames >= maxFrames) {
            break;
        }
    }

    // 会话结束：断开前“排空读”等待对端读完最后一帧应答（防 Disconnect 竞态）。
    drainAndDisconnect();
    closeUnlessPersistent();

    last.framesServed = frames;
    last.endReason = endReason;
    // 受理结论（连接层面）：凭据失败优先于正常受理上报（即使同一连接曾正常服务过帧）。
    emitVerdict(clientAuthRejected ? IpcClientVerdict::AuthFailed
                                   : IpcClientVerdict::Accepted);
    return common::Result<IpcServeResult>::Success(std::move(last));
}

common::Result<IpcServeResult> IpcSession::ServeOne(
    Handler handler, std::chrono::milliseconds acceptTimeout) {
    // 单帧语义由 ServeLoop(maxFrames=1) 承载：接受一个客户端、服务一帧、断开。
    return ServeLoop(handler, acceptTimeout, options_.ioTimeout,
                     options_.ioTimeout, 1);
}

common::Result<IpcServeResult> IpcSession::ServeSession(
    Handler handler, std::chrono::milliseconds acceptTimeout,
    std::chrono::milliseconds idleTimeout,
    std::chrono::milliseconds sessionBudget) {
    if (idleTimeout.count() <= 0 || sessionBudget.count() <= 0) {
        return common::Result<IpcServeResult>::Failure(common::Error::Validation(
            "ServeSession", L"idleTimeout 与 sessionBudget 必须为正"));
    }
    // 多帧会话：接受一个客户端后在同一连接上连续服务（帧数不设上限），结束条件
    // 见 IpcSessionEndReason（客户端关闭/帧间空闲心跳丢失/会话预算耗尽）。
    return ServeLoop(handler, acceptTimeout, idleTimeout, sessionBudget, 0);
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

common::Result<std::vector<IpcReply>> IpcRoundTripSession(
    std::shared_ptr<IpcClientBackend> backend, std::wstring_view pipePath,
    std::span<const IpcFrameRequest> requests,
    std::chrono::milliseconds timeout) {
    auto& client = *backend;
    if (requests.empty()) {
        return common::Result<std::vector<IpcReply>>::Success({});
    }

    // 连接一次，在同一连接上依次完成每帧请求-应答（连接复用多帧）。
    if (auto connected = client.Connect(pipePath, timeout); !connected) {
        return common::Result<std::vector<IpcReply>>::Failure(
            connected.ErrorValue());
    }
    std::vector<IpcReply> replies;
    replies.reserve(requests.size());
    for (const IpcFrameRequest& frame : requests) {
        if (auto written = client.WriteAll(
                MakeFrame(frame.type, frame.requestId, frame.payload),
                timeout);
            !written) {
            client.Close();
            return common::Result<std::vector<IpcReply>>::Failure(
                written.ErrorValue());
        }

        std::array<std::byte, kIpcHeaderSize> headerBytes{};
        if (auto readHeader = client.ReadAll(headerBytes, timeout);
            !readHeader) {
            client.Close();
            return common::Result<std::vector<IpcReply>>::Failure(
                readHeader.ErrorValue());
        }
        auto parsedHeader = ParseHeader(headerBytes);
        if (!parsedHeader) {
            client.Close();
            return common::Result<std::vector<IpcReply>>::Failure(
                parsedHeader.ErrorValue());
        }
        const IpcHeader header = parsedHeader.Value();

        std::vector<std::byte> replyPayload;
        if (header.payloadLength > 0) {
            replyPayload.resize(header.payloadLength);
            if (auto readPayload = client.ReadAll(replyPayload, timeout);
                !readPayload) {
                client.Close();
                return common::Result<std::vector<IpcReply>>::Failure(
                    readPayload.ErrorValue());
            }
        }

        // 应答 requestId 必须与当前请求配对（防乱序/伪造）；不匹配拒绝并中止。
        if (header.requestId != frame.requestId) {
            client.Close();
            return common::Result<std::vector<IpcReply>>::Failure(
                common::Error::Validation(
                    "IpcRoundTripSession", L"应答 requestId 与请求不匹配"));
        }
        const IpcMessageType replyType = ParseMessageType(header.type).value();
        if (replyType == IpcMessageType::Error) {
            // Error 应答：载荷首字节为错误码（空载荷按内部错误处理）。
            const IpcErrorCode code =
                replyPayload.empty()
                    ? IpcErrorCode::Internal
                    : static_cast<IpcErrorCode>(replyPayload[0]);
            client.Close();
            return common::Result<std::vector<IpcReply>>::Failure(
                common::Error::Validation(
                    "IpcRoundTripSession",
                    std::wstring(L"服务端返回错误: ") +
                        IpcErrorCodeToString(code)));
        }

        IpcReply reply;
        reply.type = replyType;
        reply.payload = std::move(replyPayload);
        replies.push_back(std::move(reply));
    }
    client.Close();
    return common::Result<std::vector<IpcReply>>::Success(std::move(replies));
}

namespace {

// 并发受理各 worker 共享的汇总与错误状态（互斥保护；每 worker 使用各自会话/实例，
// 不共享可变会话状态，仅汇总与致命错误标记共享）。
struct ConcurrentSharedState {
    std::mutex mutex;
    IpcConcurrentSummary summary;
    bool fatalError = false;
};

// 单个 worker：用自己（工厂创建的）会话/管道实例在窗口内循环——接受一个客户端 ->
// 在同一连接上按会话语义服务多帧（ServeSession，frameIdle 为帧间空闲上限）-> 会话
// 结束继续接受下一客户端，直至窗口到期（每次迭代以剩余时间重算接受窗口与预算，
// 最后到达的客户端也能获得至少 frameIdle 的服务时长，窗口溢出有界）。无客户端等待
// 超时/连接即断的 0 帧会话属正常活动缺失：不计数，继续循环到窗口到期自然退出；
// 其余会话级失败（拒绝/非法帧/读写失败）计数后继续（一个坏客户端不中断他人）。
// 线程体整体捕获异常：任何意外都汇入 fatalError，由 RunConcurrentServer join 后上报。
void RunConcurrentWorker(
    std::shared_ptr<IpcSession> session, IpcSession::Handler handler,
    std::chrono::steady_clock::time_point deadline,
    std::chrono::milliseconds frameIdle,
    std::shared_ptr<ConcurrentSharedState> shared) noexcept {
    try {
        for (;;) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                break;
            }
            auto served =
                session->ServeSession(handler, remaining, frameIdle,
                                      remaining + frameIdle);
            if (!served) {
                const auto& error = served.ErrorValue();
                const bool noActivity =
                    error.domain == common::ErrorDomain::Win32 &&
                    (error.code == ERROR_TIMEOUT ||
                     error.code == ERROR_BROKEN_PIPE ||
                     error.code == ERROR_NO_DATA);
                if (noActivity) {
                    continue;
                }
                std::lock_guard<std::mutex> lock(shared->mutex);
                ++shared->summary.failedSessions;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->summary.sessions.push_back(served.Value());
                ++shared->summary.clientsServed;
            }
        }
        session->Close(); // 窗口到期：释放本 worker 的管道实例（幂等）
    } catch (...) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->fatalError = true;
    }
}

} // namespace

common::Result<IpcConcurrentSummary> RunConcurrentServer(
    std::size_t instances, std::chrono::milliseconds window,
    std::chrono::milliseconds frameIdle, IpcSession::Handler handler,
    const std::function<std::shared_ptr<IpcSession>()>& sessionFactory) {
    if (instances == 0 || window.count() <= 0 || frameIdle.count() <= 0) {
        return common::Result<IpcConcurrentSummary>::Failure(
            common::Error::Validation(
                "RunConcurrentServer",
                L"instances/window/frameIdle 必须为正"));
    }
    if (!sessionFactory) {
        return common::Result<IpcConcurrentSummary>::Failure(
            common::Error::Validation(
                "RunConcurrentServer", L"sessionFactory 不能为空"));
    }

    auto shared = std::make_shared<ConcurrentSharedState>();
    shared->summary.workers = instances;
    const auto deadline = std::chrono::steady_clock::now() + window;

    // 启动 N 个有界 worker（每 worker = 独立实例 + 独立线程）；窗口到期全部 join，
    // 无脱逸/后台线程，调用返回即回收。
    std::vector<std::thread> threads;
    threads.reserve(instances);
    for (std::size_t i = 0; i < instances; ++i) {
        threads.emplace_back([&]() {
            auto session = sessionFactory();
            if (!session) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->fatalError = true; // 工厂失败视为编排致命错误
                return;
            }
            RunConcurrentWorker(std::move(session), handler, deadline,
                                frameIdle, shared);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (shared->fatalError) {
        return common::Result<IpcConcurrentSummary>::Failure(
            common::Error::Validation(
                "RunConcurrentServer", L"worker 异常/会话工厂失败"));
    }
    return common::Result<IpcConcurrentSummary>::Success(
        std::move(shared->summary));
}

common::Result<IpcPeriodicReportSummary> RunPeriodicReporter(
    std::shared_ptr<IpcClientBackend> backend,
    const IpcPeriodicReportOptions& options,
    const std::function<common::Result<IpcFrameRequest>()>& requestFactory) {
    if (options.window.count() <= 0 || options.interval.count() <= 0 ||
        options.ioTimeout.count() <= 0 || options.maxConnectAttempts == 0) {
        return common::Result<IpcPeriodicReportSummary>::Failure(
            common::Error::Validation(
                "RunPeriodicReporter",
                L"window/interval/ioTimeout 必须为正且 maxConnectAttempts>0"));
    }
    if (!requestFactory) {
        return common::Result<IpcPeriodicReportSummary>::Failure(
            common::Error::Validation(
                "RunPeriodicReporter", L"requestFactory 不能为空"));
    }

    IpcPeriodicReportSummary summary;
    const auto deadline = std::chrono::steady_clock::now() + options.window;
    auto nextSlot = std::chrono::steady_clock::now();
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break; // 窗口到期必然返回（有界）
        }
        if (now < nextSlot) {
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nextSlot - now));
            continue;
        }
        // 生成本次上报帧（每帧独立真实观测；requestId 由调用方递增）。
        auto request = requestFactory();
        if (!request) {
            return common::Result<IpcPeriodicReportSummary>::Failure(
                request.ErrorValue()); // 采样失败：致命，原样上报
        }
        bool delivered = false;
        for (std::size_t attempt = 0; attempt < options.maxConnectAttempts;
             ++attempt) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            auto reply = IpcRoundTrip(
                backend, options.pipePath, request.Value().type,
                request.Value().payload, request.Value().requestId,
                options.ioTimeout);
            if (reply) {
                ++summary.reportsSent;
                summary.lastReplyAck =
                    reply.Value().type == IpcMessageType::Ack;
                delivered = true;
                break;
            }
            // 连接/传输失败（宿主离线/Safe Mode 暂停/被拒）：退避后重试本周期。
            if (attempt + 1 < options.maxConnectAttempts) {
                std::this_thread::sleep_for(options.reconnectBackoff);
            }
        }
        if (!delivered) {
            ++summary.connectFailures; // 可恢复失败：继续下一周期
        }
        nextSlot += options.interval;
        // 追赶保护：周期执行过慢时以“当前 + interval”对齐，避免突发密集上报。
        if (nextSlot <= std::chrono::steady_clock::now()) {
            nextSlot = std::chrono::steady_clock::now() + options.interval;
        }
    }
    backend->Close();
    return common::Result<IpcPeriodicReportSummary>::Success(
        std::move(summary));
}

} // namespace optimizer::ipc
