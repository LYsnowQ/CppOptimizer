#include "ipc/ipc_protocol.hpp"

namespace optimizer::ipc {

namespace {

// 小端读取 4 字节无符号。
std::uint32_t ReadU32LE(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i])
                 << (8 * i);
    }
    return value;
}

// 小端写入 4 字节无符号。
void WriteU32LE(std::uint32_t value, std::byte* out) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
}

} // namespace

const wchar_t* MessageTypeToString(IpcMessageType type) noexcept {
    switch (type) {
        case IpcMessageType::Ping:
            return L"Ping";
        case IpcMessageType::Ack:
            return L"Ack";
        case IpcMessageType::FactsSnapshot:
            return L"FactsSnapshot";
        case IpcMessageType::Error:
            return L"Error";
    }
    return L"Unknown";
}

std::optional<IpcMessageType> ParseMessageType(std::uint8_t value) noexcept {
    switch (value) {
        case static_cast<std::uint8_t>(IpcMessageType::Ping):
            return IpcMessageType::Ping;
        case static_cast<std::uint8_t>(IpcMessageType::Ack):
            return IpcMessageType::Ack;
        case static_cast<std::uint8_t>(IpcMessageType::FactsSnapshot):
            return IpcMessageType::FactsSnapshot;
        case static_cast<std::uint8_t>(IpcMessageType::Error):
            return IpcMessageType::Error;
    }
    return std::nullopt;
}

IpcHeader MakeIpcHeader(IpcMessageType type, std::uint32_t payloadLength,
                        std::uint32_t requestId) noexcept {
    IpcHeader header;
    header.type = static_cast<std::uint8_t>(type);
    header.payloadLength = payloadLength;
    header.requestId = requestId;
    return header;
}

common::Result<void> ValidateIpcHeader(const IpcHeader& header) noexcept {
    if (header.magic != kIpcMagic) {
        return common::Result<void>::Failure(common::Error::Validation(
            "ValidateIpcHeader", L"帧魔数不匹配，拒绝"));
    }
    if (header.version != kIpcVersion) {
        return common::Result<void>::Failure(common::Error::Validation(
            "ValidateIpcHeader", L"不支持的协议版本"));
    }
    if (!ParseMessageType(header.type)) {
        return common::Result<void>::Failure(common::Error::Validation(
            "ValidateIpcHeader", L"未知消息类型，拒绝"));
    }
    if (header.reserved != 0) {
        return common::Result<void>::Failure(common::Error::Validation(
            "ValidateIpcHeader", L"预留字段非零，拒绝"));
    }
    if (header.payloadLength > kMaxPayloadLength) {
        return common::Result<void>::Failure(common::Error::Validation(
            "ValidateIpcHeader", L"载荷超过上限"));
    }
    return common::Result<void>::Success();
}

void SerializeHeader(const IpcHeader& header,
                     std::array<std::byte, kIpcHeaderSize>& out) noexcept {
    WriteU32LE(header.magic, &out[0]);
    out[4] = static_cast<std::byte>(header.version);
    out[5] = static_cast<std::byte>(header.type);
    out[6] = static_cast<std::byte>(header.reserved & 0xFF);
    out[7] = static_cast<std::byte>((header.reserved >> 8) & 0xFF);
    WriteU32LE(header.payloadLength, &out[8]);
    WriteU32LE(header.requestId, &out[12]);
}

common::Result<IpcHeader> ParseHeader(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != kIpcHeaderSize) {
        return common::Result<IpcHeader>::Failure(common::Error::Validation(
            "ParseHeader", L"帧头长度必须为 16 字节"));
    }
    IpcHeader header;
    header.magic = ReadU32LE(bytes, 0);
    header.version = static_cast<std::uint8_t>(bytes[4]);
    header.type = static_cast<std::uint8_t>(bytes[5]);
    header.reserved = static_cast<std::uint16_t>(bytes[6]) |
                      (static_cast<std::uint16_t>(bytes[7]) << 8);
    header.payloadLength = ReadU32LE(bytes, 8);
    header.requestId = ReadU32LE(bytes, 12);

    if (const auto valid = ValidateIpcHeader(header); !valid) {
        return common::Result<IpcHeader>::Failure(valid.ErrorValue());
    }
    return common::Result<IpcHeader>::Success(header);
}

} // namespace optimizer::ipc
