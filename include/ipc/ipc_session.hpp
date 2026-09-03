#pragma once

#include "common/error.hpp"
#include "ipc/ipc_protocol.hpp"
#include "ipc/ipc_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace optimizer::ipc {

// 帧级错误码（Error 应答载荷首字节）。
enum class IpcErrorCode : std::uint8_t {
    InvalidHeader = 0x01,     // 帧头魔数/版本/类型/预留/长度非法
    UnsupportedType = 0x03,   // 处理器不支持该消息类型
    HandlerFailed = 0x04,     // 应用层处理器失败
    Internal = 0x05,          // 内部错误
    InvalidFacts = 0x06,      // FactsSnapshot 载荷违反 CPOPFACTS/1 契约
    UnauthorizedClient = 0x07, // 会话级身份裁决拒绝（非交互会话/不可识别客户端）
    AuthFailed = 0x08 // 凭据校验失败（agent_token 缺失/不匹配，见 expectedToken）
};

// 错误码名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* IpcErrorCodeToString(IpcErrorCode code) noexcept;

// 服务端收到的请求（帧已校验；载荷 <= kMaxPayloadLength）。
struct IpcRequest {
    IpcMessageType type = IpcMessageType::Ping;
    std::uint32_t requestId = 0;
    std::vector<std::byte> payload;
    std::uint32_t clientPid = 0;       // 客户端进程 ID（查询失败为 0）
    std::uint32_t clientSessionId = 0; // 客户端会话 ID（查询失败为 0）
};

// 应答（服务端回写；载荷必须 <= kMaxPayloadLength，超限按内部错误拒绝）。
struct IpcReply {
    IpcMessageType type = IpcMessageType::Ack;
    std::vector<std::byte> payload;
};

// 会话（连接）结束原因（ServeSession 多帧服务）。
enum class IpcSessionEndReason : std::uint8_t {
    ClientClosed = 0, // 客户端完成多帧后主动关闭连接（正常结束）
    IdleTimeout = 1,  // 帧间空闲超时：客户端静默超过 idleTimeout（心跳丢失）
    SessionBudget = 2 // 会话总预算耗尽（调用方窗口到期）
};

// 结束原因名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* IpcSessionEndReasonToString(
    IpcSessionEndReason reason) noexcept;

// 一次会话结果摘要。
struct IpcServeResult {
    IpcMessageType requestType = IpcMessageType::Ping;
    IpcMessageType replyType = IpcMessageType::Ack; // 实际写出的应答类型（Ack/Error）
    std::uint32_t requestId = 0;
    std::uint32_t clientPid = 0;
    std::uint32_t clientSessionId = 0;
    std::wstring clientUserSid; // 客户端用户 SID（未知为空串）
    std::uint32_t payloadBytes = 0;
    // 本调用成功服务的帧数（ServeOne 恒为 1；ServeSession 为该连接内已服务帧数）。
    std::uint32_t framesServed = 1;
    // 会话结束原因（ServeSession 填写；ServeOne 单帧语义视为 ClientClosed）。
    IpcSessionEndReason endReason = IpcSessionEndReason::ClientClosed;
};

// 客户端身份（服务端从传输层记录/查询，见 ipc_transport.hpp）。
struct IpcClientIdentity {
    std::uint32_t pid = 0;       // 客户端进程 ID（查询失败为 0）
    std::uint32_t sessionId = 0; // 客户端会话 ID（查询失败为 0）
    // 客户端用户 SID（IPC-006 访问令牌只读查询；未知/跨用户不可读为空串）。
    std::wstring userSid;
};

// 单客户端会话服务端：创建/监听 -> 接受一个客户端 -> 逐帧严格校验（未知版本/
// 类型/超长载荷 -> Error 应答并断开，不做宽松转换）-> 会话级身份裁决（默认要求
// 可识别 PID 与交互会话；拒绝 -> Error(UnauthorizedClient) 并断开）-> 用户 SID
// 授权白名单（配置时）-> 调用处理器 -> 写回应答。处理器失败 -> Error 应答
// （失败不伪装成功）。
// 实例可复用（每轮重新 CreateAndListen/AcceptClient）。
// persistentAccept（SVC-003 连续受理）：同一管道实例在 ServeOne/ServeSession 之间
// 保持监听，可连续服务多个客户端，由 Close() 结束。
// ServeOne 每客户端一帧；ServeSession 在同一连接上连续服务多帧（连接复用/心跳，
// 结束条件见 IpcSessionEndReason）。
// 多实例并发（IPC-009）：RunConcurrentServer 以 N 个独立实例/线程并发受理并服务多个
// 客户端（每个连接在自己的实例上按会话语义服务），由窗口到期 join 回收。
class IpcSession {
public:
    // 应用层处理器：根据请求填写应答；返回失败表示拒绝（应答 Error）。
    using Handler =
        std::function<common::Result<void>(const IpcRequest&, IpcReply&)>;

    // 会话级身份裁决：受理一帧前按客户端身份决定是否放行。
    // 返回失败表示拒绝（回 Error(UnauthorizedClient) 应答并断开）。
    using ClientGate =
        std::function<common::Result<void>(const IpcClientIdentity& client)>;

    // 默认裁决（Options 缺省启用）：客户端必须身份可识别（pid != 0）且位于
    // 交互会话（sessionId != 0）。会话 0（服务）不是合法的 Agent 上报方。
    [[nodiscard]] static common::Result<void> DefaultClientGate(
        const IpcClientIdentity& client) noexcept;

    // 放行一切客户端（关闭身份裁决；仅测试/隔离场景使用）。
    [[nodiscard]] static common::Result<void> AllowAllClientGate(
        const IpcClientIdentity&) noexcept;

    struct Options {
        std::chrono::milliseconds ioTimeout =
            std::chrono::milliseconds(3000); // 单次读/写超时
        ClientGate clientGate = DefaultClientGate; // 身份裁决（默认启用）
        // 连续受理（SVC-003）：开启后同一管道实例跨 ServeOne 保持监听——上一客户端
        // 服务完仅断开不释放，下一次 ServeOne 直接等待新客户端，调用间隙无监听空窗；
        // 接受超时返回 ERROR_TIMEOUT 且不关闭实例；结束由调用方调 Close()。
        // 默认 false 保持 IPC-001~006 的“单次服务即释放”语义。
        bool persistentAccept = false;
        // 客户端用户 SID 授权白名单（IPC-006）：非空时要求客户端 SID（传输层
        // 访问令牌只读查询）大小写不敏感命中其一，否则回 Error(UnauthorizedClient)
        // 并断开。为空表示不启用 SID 授权（兼容 IPC-004/005 行为）。
        std::vector<std::wstring> allowedClientSids;
        // 会话凭据（IPC-005）：非空时默认处理器要求 FactsSnapshot 载荷携带匹配
        // 的 agent_token 事实，缺失/不匹配回 Error(AuthFailed)；为空表示不启用
        // （沿用 IPC-002/003 行为）。真实供给见 ipc_credentials.hpp（IPC-007：
        // 每用户私有 ACL 存储 + 轮换），此处为程序内注入点（CLI/宿主从存储加载）。
        std::wstring expectedToken;
    };

    explicit IpcSession(std::shared_ptr<IpcServerBackend> backend,
                        Options options = {});

    // 释放管道实例（幂等）。persistentAccept 会话结束（窗口到期/停止）时调用；
    // 非持续模式每次 ServeOne 已自行释放，此处为空操作（防御性）。
    void Close() noexcept;

    // 服务至多一个客户端的一帧。acceptTimeout 到期无客户端 -> ERROR_TIMEOUT；
    // 帧非法/身份裁决拒绝/处理器失败/传输失败 -> 对应错误（已发出 Error 应答的
    // 路径一并上报）。persistentAccept 时实例跨调用保持，可反复调用以服务多个客户端。
    [[nodiscard]] common::Result<IpcServeResult> ServeOne(
        Handler handler, std::chrono::milliseconds acceptTimeout);

    // 同一连接上的多帧服务（连接复用/心跳）：接受一个客户端后在其连接上连续服务
    // 多帧，每帧独立执行与 ServeOne 相同的 读帧-严格校验-裁决-处理器-应答 顺序，
    // 直至客户端关闭（ClientClosed）、帧间空闲超过 idleTimeout（IdleTimeout，心跳
    // 丢失）或会话总预算 sessionBudget 耗尽（SessionBudget）。acceptTimeout 为接受
    // 窗口（无客户端 -> ERROR_TIMEOUT）。返回结果含 framesServed（已服务帧数）与
    // endReason。任一帧失败（非法帧/裁决拒绝/处理器失败/传输失败）即终止会话并返回
    // 失败（已发出 Error 应答的路径一并上报；与 ServeOne 严格语义一致）。
    // persistentAccept 语义同 ServeOne：会话结束后是否保留监听实例由 Close 控制。
    [[nodiscard]] common::Result<IpcServeResult> ServeSession(
        Handler handler, std::chrono::milliseconds acceptTimeout,
        std::chrono::milliseconds idleTimeout,
        std::chrono::milliseconds sessionBudget);

    // 默认处理器（无凭据要求）：Ping -> Ack；FactsSnapshot -> 解析 CPOPFACTS/1 载荷并过 v1
    // 键语义白名单（ipc_facts.hpp），合法则 Ack（载荷为紧凑摘要文本），语法或
    // schema 任一违反则 Error(InvalidFacts) 应答（不宽松接受）；其余请求类型 ->
    // 拒绝（UnsupportedType）。处理器只应答，不执行任何客户端请求的系统动作。
    [[nodiscard]] static common::Result<void> DefaultHandler(
        const IpcRequest& request, IpcReply& reply);

    // 带会话凭据要求的默认处理器（IPC-005）：expectedToken 非空时，FactsSnapshot
    // 在语法/schema 通过后还需携带完全匹配的 agent_token 事实，否则回
    // Error(AuthFailed)（不伪装成功）。expectedToken 为空等价于两参版本。
    [[nodiscard]] static common::Result<void> DefaultHandler(
        const IpcRequest& request, IpcReply& reply,
        std::wstring_view expectedToken);

private:
    // 服务循环内核：CreateAndListen -> AcceptClient -> 逐帧服务。
    // maxFrames==0 表示不设帧数上限（由客户端关闭/帧间空闲/会话预算结束，即
    // ServeSession）；>0 表示服务满该帧数即正常结束（ServeOne 的 1 帧语义）。
    [[nodiscard]] common::Result<IpcServeResult> ServeLoop(
        Handler handler, std::chrono::milliseconds acceptTimeout,
        std::chrono::milliseconds idleTimeout,
        std::chrono::milliseconds sessionBudget, std::uint32_t maxFrames);

    std::shared_ptr<IpcServerBackend> backend_;
    Options options_;
};

// 客户端往返：连接 -> 发送一帧 -> 读取应答帧 -> 关闭。
// 应答为 Error 类型或 requestId 与请求不配对时返回失败（失败不伪装成功）。
[[nodiscard]] common::Result<IpcReply> IpcRoundTrip(
    std::shared_ptr<IpcClientBackend> backend, std::wstring_view pipePath,
    IpcMessageType type, std::span<const std::byte> payload,
    std::uint32_t requestId, std::chrono::milliseconds timeout);

// 客户端单连接会话中的一帧请求（连接复用多帧，见 IpcRoundTripSession）。
struct IpcFrameRequest {
    IpcMessageType type = IpcMessageType::Ping;
    std::uint32_t requestId = 0;
    std::vector<std::byte> payload;
};

// 复用同一连接的多次往返（多帧）：连接一次，依次发送 requests 每帧并读取配对
// 应答（requestId 配对、Error 应答不伪装成功，与 IpcRoundTrip 语义一致）；任一帧
// 失败即中止并关闭连接返回失败。全部完成返回与 requests 一一对应的应答列表。
[[nodiscard]] common::Result<std::vector<IpcReply>> IpcRoundTripSession(
    std::shared_ptr<IpcClientBackend> backend, std::wstring_view pipePath,
    std::span<const IpcFrameRequest> requests,
    std::chrono::milliseconds timeout);

// 一次并发受理（多实例）的汇总。
struct IpcConcurrentSummary {
    std::size_t workers = 0;             // 启动的 worker（管道实例）数
    std::size_t clientsServed = 0;       // 服务完成的客户端会话数（含 Error 回执帧的会话）
    std::size_t failedSessions = 0;      // 会话级失败数（客户端即连即断/被拒/读失败等）
    std::vector<IpcServeResult> sessions; // 各客户端会话摘要（完成顺序不定）
};

// 多实例并发受理（IPC-009）：以 instances 个独立管道实例（各自线程 + 各自 IpcSession）
// 在 window 窗口内并发受理并服务客户端。每个 worker 循环：接受一个客户端 -> 在同一
// 连接上按会话语义服务多帧（frameIdle 为帧间空闲/心跳丢失上限，复用 ServeSession）->
// 会话结束后继续接受下一客户端，直至窗口到期（无客户端等待超时属正常结束）。窗口到期
// join 全部 worker 并由各会话 Close 实例：无脱逸/后台线程，调用返回后线程全部回收。
// sessionFactory 为每个 worker 构造会话（含各自后端与 Options，如 persistentAccept=
// true 使实例跨会话保持监听），便于测试注入 fake；handler 为空使用默认处理器。
// instances/window/frameIdle 非正 -> Validation 拒绝（不启动任何线程）。
[[nodiscard]] common::Result<IpcConcurrentSummary> RunConcurrentServer(
    std::size_t instances, std::chrono::milliseconds window,
    std::chrono::milliseconds frameIdle, IpcSession::Handler handler,
    const std::function<std::shared_ptr<IpcSession>()>& sessionFactory);

} // namespace optimizer::ipc
