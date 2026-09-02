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
    InvalidHeader = 0x01,   // 帧头魔数/版本/类型/预留/长度非法
    UnsupportedType = 0x03, // 处理器不支持该消息类型
    HandlerFailed = 0x04,   // 应用层处理器失败
    Internal = 0x05,        // 内部错误
    InvalidFacts = 0x06     // FactsSnapshot 载荷违反 CPOPFACTS/1 契约
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
    std::uint32_t payloadBytes = 0;
};

// 单客户端会话服务端：创建/监听 -> 接受一个客户端 -> 读取一帧 ->
// 严格校验（未知版本/类型/超长载荷 -> Error 应答并断开，不做宽松转换）
// -> 调用处理器 -> 写回应答 -> 断开。处理器失败 -> Error 应答（失败不伪装成功）。
// 实例可复用（每轮重新 CreateAndListen/AcceptClient）。
class IpcSession {
public:
    // 应用层处理器：根据请求填写应答；返回失败表示拒绝（应答 Error）。
    using Handler =
        std::function<common::Result<void>(const IpcRequest&, IpcReply&)>;

    struct Options {
        std::chrono::milliseconds ioTimeout =
            std::chrono::milliseconds(3000); // 单次读/写超时
    };

    explicit IpcSession(std::shared_ptr<IpcServerBackend> backend,
                        Options options = {});

    // 服务至多一个客户端的一帧。acceptTimeout 到期无客户端 -> ERROR_TIMEOUT；
    // 帧非法/处理器失败/传输失败 -> 对应错误（已发出 Error 应答的路径一并上报）。
    [[nodiscard]] common::Result<IpcServeResult> ServeOne(
        Handler handler, std::chrono::milliseconds acceptTimeout);

    // 默认处理器：Ping -> Ack；FactsSnapshot -> 解析 CPOPFACTS/1 载荷并过 v1
    // 键语义白名单（ipc_facts.hpp），合法则 Ack（载荷为紧凑摘要文本），语法或
    // schema 任一违反则 Error(InvalidFacts) 应答（不宽松接受）；其余请求类型 ->
    // 拒绝（UnsupportedType）。处理器只应答，不执行任何客户端请求的系统动作。
    [[nodiscard]] static common::Result<void> DefaultHandler(
        const IpcRequest& request, IpcReply& reply);

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
