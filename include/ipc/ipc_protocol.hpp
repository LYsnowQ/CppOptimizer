#pragma once

#include "common/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace optimizer::ipc {

// 帧魔数（ASCII 'CPOP' 小端存储）。任一帧头必须匹配，不匹配按非法帧拒绝。
inline constexpr std::uint32_t kIpcMagic = 0x43504F50;

// 协议版本。仅接受本版本；未知版本拒绝（不做宽松转换）。
inline constexpr std::uint8_t kIpcVersion = 1;

// 单帧载荷上限（字节）。帧头 payloadLength 超过上限即拒绝。
inline constexpr std::uint32_t kMaxPayloadLength = 4096;

// 帧头字节数（固定 16 字节，小端）。
inline constexpr std::size_t kIpcHeaderSize = 16;

// 消息类型（帧头 type 字段）。新增类型需显式扩展并同步 ParseMessageType。
enum class IpcMessageType : std::uint8_t {
    Ping = 0x01,          // 客户端 -> 服务端：连通性探针（无载荷）
    Ack = 0x02,           // 服务端 -> 客户端：成功应答
    FactsSnapshot = 0x03, // 客户端 -> 服务端：事实快照（载荷为应用层编码）
    Error = 0x04          // 服务端 -> 客户端：错误应答（载荷首字节为 IpcErrorCode）
};

// 类型名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* MessageTypeToString(IpcMessageType type) noexcept;

// 严格解析类型字节：仅接受已定义枚举值，其余返回 nullopt。
[[nodiscard]] std::optional<IpcMessageType> ParseMessageType(
    std::uint8_t value) noexcept;

// 帧头（固定 16 字节小端布局，见 SerializeHeader）。
struct IpcHeader {
    std::uint32_t magic = kIpcMagic;
    std::uint8_t version = kIpcVersion;
    std::uint8_t type = static_cast<std::uint8_t>(IpcMessageType::Ping);
    std::uint16_t reserved = 0; // 预留字段，必须为 0（非零拒绝）
    std::uint32_t payloadLength = 0;
    std::uint32_t requestId = 0;
};

// 构造帧头（不做校验；校验走 ValidateIpcHeader）。
[[nodiscard]] IpcHeader MakeIpcHeader(
    IpcMessageType type, std::uint32_t payloadLength,
    std::uint32_t requestId) noexcept;

// 帧头校验（纯函数）：魔数/版本/类型/预留/载荷长度全项通过才成功；
// 任一不合法返回 Validation（帧级拒绝，不伪装成功）。
[[nodiscard]] common::Result<void> ValidateIpcHeader(
    const IpcHeader& header) noexcept;

// 序列化帧头到固定 kIpcHeaderSize 字节缓冲区（小端）。
void SerializeHeader(const IpcHeader& header,
                     std::array<std::byte, kIpcHeaderSize>& out) noexcept;

// 从字节解析帧头：长度必须恰为 kIpcHeaderSize，并完整校验帧头字段；
// 失败返回 Validation。
[[nodiscard]] common::Result<IpcHeader> ParseHeader(
    std::span<const std::byte> bytes) noexcept;

} // namespace optimizer::ipc
