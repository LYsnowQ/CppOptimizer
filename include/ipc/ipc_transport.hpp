#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace optimizer::ipc {

// 命名管道服务端传输后端（可注入 fake 单测）。
// 生命周期：CreateAndListen -> [AcceptClient -> ReadAll/WriteAll ...
// -> DisconnectClient] -> Close。同一管道实例可断开后复用接受新客户端；
// Close 后不可再用（重建后端实例）。
class IpcServerBackend {
public:
    virtual ~IpcServerBackend() = default;

    // 创建并监听命名管道（显式 SDDL：SYSTEM / Administrators / 交互用户）。
    // 失败返回 Win32 错误（如管道名被占用）。
    [[nodiscard]] virtual common::Result<void> CreateAndListen() = 0;

    // 等待一个客户端连接；timeout 到期仍未连接返回
    // Error(FromWin32(ERROR_TIMEOUT))。timeout 为 0 时立即尝试。
    [[nodiscard]] virtual common::Result<void> AcceptClient(
        std::chrono::milliseconds timeout) = 0;

    // 读取精确 buffer.size() 字节；对端关闭/超时/读失败返回错误（不伪装成功）。
    [[nodiscard]] virtual common::Result<void> ReadAll(
        std::span<std::byte> buffer,
        std::chrono::milliseconds timeout) = 0;

    // 写入 buffer 全部字节；超时/写失败返回错误。
    [[nodiscard]] virtual common::Result<void> WriteAll(
        std::span<const std::byte> buffer,
        std::chrono::milliseconds timeout) = 0;

    // 断开当前客户端（保留管道句柄，可再次 AcceptClient）；未连接时为空操作。
    [[nodiscard]] virtual common::Result<void> DisconnectClient() = 0;

    // 关闭并释放管道；幂等。之后不可再使用。
    virtual void Close() noexcept = 0;

    // 当前客户端 PID（GetNamedPipeClientProcessId；未知/未连接返回 0）。
    [[nodiscard]] virtual std::uint32_t ClientPid() const noexcept = 0;

    // 当前客户端会话 ID（ProcessIdToSessionId；未知/未连接返回 0）。
    [[nodiscard]] virtual std::uint32_t ClientSessionId() const noexcept = 0;

    // 当前客户端用户 SID（IPC-006，访问令牌只读查询：OpenProcess(
    // PROCESS_QUERY_INFORMATION) -> OpenProcessToken(TOKEN_QUERY) ->
    // GetTokenInformation(TokenUser) -> ConvertSidToStringSidW）。同用户进程可读，
    // 无特权/跨用户不可读时返回空串（未知身份，由授权策略决定是否拒绝）。
    [[nodiscard]] virtual std::wstring ClientUserSid() const = 0;
};

// 命名管道客户端传输后端（可注入 fake 单测）。
class IpcClientBackend {
public:
    virtual ~IpcClientBackend() = default;

    // 连接命名管道；管道忙时等待重试直到 timeout（ERROR_PIPE_BUSY 语义）。
    [[nodiscard]] virtual common::Result<void> Connect(
        std::wstring_view pipePath, std::chrono::milliseconds timeout) = 0;

    // 写入全部字节；超时/写失败返回错误。
    [[nodiscard]] virtual common::Result<void> WriteAll(
        std::span<const std::byte> buffer,
        std::chrono::milliseconds timeout) = 0;

    // 读取精确字节数；对端关闭/超时/读失败返回错误。
    [[nodiscard]] virtual common::Result<void> ReadAll(
        std::span<std::byte> buffer,
        std::chrono::milliseconds timeout) = 0;

    // 关闭连接；幂等。
    virtual void Close() noexcept = 0;
};

// 构造 Win32 服务端后端。pipeName 为完整命名管道名（如 L"\\\\.\\pipe\\CppOptimizerIpc"）。
[[nodiscard]] std::shared_ptr<IpcServerBackend> CreateWin32ServerBackend(
    std::wstring pipeName);

// 构造 Win32 客户端后端。
[[nodiscard]] std::shared_ptr<IpcClientBackend> CreateWin32ClientBackend();

} // namespace optimizer::ipc
