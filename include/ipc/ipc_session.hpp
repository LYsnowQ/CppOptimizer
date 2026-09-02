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

// 一次会话结果摘要。
struct IpcServeResult {
    IpcMessageType requestType = IpcMessageType::Ping;
    IpcMessageType replyType = IpcMessageType::Ack; // 实际写出的应答类型（Ack/Error）
    std::uint32_t requestId = 0;
    std::uint32_t clientPid = 0;
    std::uint32_t clientSessionId = 0;
    std::wstring clientUserSid; // 客户端用户 SID（未知为空串）
    std::uint32_t payloadBytes = 0;
};

// 客户端身份（服务端从传输层记录/查询，见 ipc_transport.hpp）。
struct IpcClientIdentity {
    std::uint32_t pid = 0;       // 客户端进程 ID（查询失败为 0）
    std::uint32_t sessionId = 0; // 客户端会话 ID（查询失败为 0）
    // 客户端用户 SID（IPC-006 访问令牌只读查询；未知/跨用户不可读为空串）。
    std::wstring userSid;
};

// 单客户端会话服务端：创建/监听 -> 接受一个客户端 -> 读取一帧 ->
// 严格校验（未知版本/类型/超长载荷 -> Error 应答并断开，不做宽松转换）
// -> 会话级身份裁决（默认要求可识别 PID 与交互会话；拒绝 -> Error(UnauthorizedClient)
//    并断开）-> 调用处理器 -> 写回应答 -> 断开。处理器失败 -> Error 应答
// （失败不伪装成功）。实例可复用（每轮重新 CreateAndListen/AcceptClient）。
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
        // 客户端用户 SID 授权白名单（IPC-006）：非空时要求客户端 SID（传输层
        // 访问令牌只读查询）大小写不敏感命中其一，否则回 Error(UnauthorizedClient)
        // 并断开。为空表示不启用 SID 授权（兼容 IPC-004/005 行为）。
        std::vector<std::wstring> allowedClientSids;
        // 会话凭据（IPC-005，demo 层共享秘密）：非空时默认处理器要求
        // FactsSnapshot 载荷携带匹配的 agent_token 事实，缺失/不匹配回
        // Error(AuthFailed)；为空表示不启用（沿用 IPC-002/003 行为）。
        // 真实供给（按用户派生/ACL 注入）属 Service/Agent 切片。
        std::wstring expectedToken;
    };

    explicit IpcSession(std::shared_ptr<IpcServerBackend> backend,
                        Options options = {});

    // 服务至多一个客户端的一帧。acceptTimeout 到期无客户端 -> ERROR_TIMEOUT；
    // 帧非法/身份裁决拒绝/处理器失败/传输失败 -> 对应错误（已发出 Error 应答的
    // 路径一并上报）。
    [[nodiscard]] common::Result<IpcServeResult> ServeOne(
        Handler handler, std::chrono::milliseconds acceptTimeout);

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
    std::shared_ptr<IpcServerBackend> backend_;
    Options options_;
};

// 客户端往返：连接 -> 发送一帧 -> 读取应答帧 -> 关闭。
// 应答为 Error 类型或 requestId 与请求不配对时返回失败（失败不伪装成功）。
[[nodiscard]] common::Result<IpcReply> IpcRoundTrip(
    std::shared_ptr<IpcClientBackend> backend, std::wstring_view pipePath,
    IpcMessageType type, std::span<const std::byte> payload,
    std::uint32_t requestId, std::chrono::milliseconds timeout);

} // namespace optimizer::ipc
