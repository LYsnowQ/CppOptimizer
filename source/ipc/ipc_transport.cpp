#include "ipc/ipc_protocol.hpp"
#include "ipc/ipc_transport.hpp"

#include "common/unique_resource.hpp"

#include <sddl.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

namespace optimizer::ipc {

namespace {

// 显式 SDDL：仅 SYSTEM、内置管理员与交互用户可访问；保护 DACL（不继承）。
constexpr wchar_t kPipeSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)";

// 等待重叠操作完成。超时或等待失败时取消未完成操作并等待其真正结束
// （避免悬空引用已销毁的 OVERLAPPED），返回 false。
bool WaitOverlapped(HANDLE pipe, OVERLAPPED& ov,
                    std::chrono::milliseconds remaining) noexcept {
    const DWORD result =
        ::WaitForSingleObject(ov.hEvent, static_cast<DWORD>(remaining.count()));
    if (result == WAIT_OBJECT_0) {
        return true;
    }
    ::CancelIoEx(pipe, &ov);
    ::WaitForSingleObject(ov.hEvent, INFINITE); // 等待取消完成，I/O 不再引用 ov
    return false;
}

// 带超时的精确读（返回 Win32 错误码；ERROR_SUCCESS 表示成功）。
// 对端关闭返回 ERROR_BROKEN_PIPE；超时返回 ERROR_TIMEOUT。
std::uint32_t ReadAllOverlapped(HANDLE pipe, std::span<std::byte> buffer,
                                std::chrono::milliseconds timeout) noexcept {
    if (buffer.empty()) {
        return ERROR_SUCCESS;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            return ::GetLastError();
        }
        common::UniqueHandle eventGuard(ov.hEvent);

        DWORD bytesRead = 0;
        const BOOL ok = ::ReadFile(pipe, buffer.data() + offset,
                                   static_cast<DWORD>(buffer.size() - offset),
                                   &bytesRead, &ov);
        if (!ok) {
            const std::uint32_t error = ::GetLastError();
            if (error != ERROR_IO_PENDING) {
                return error;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || !WaitOverlapped(pipe, ov, remaining)) {
                return ERROR_TIMEOUT;
            }
            if (!::GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
                return ::GetLastError();
            }
        }
        if (bytesRead == 0) {
            return ERROR_BROKEN_PIPE; // 对端已关闭
        }
        offset += bytesRead;
    }
    return ERROR_SUCCESS;
}

// 带超时的精确写（返回 Win32 错误码；ERROR_SUCCESS 表示成功）。
// 对端关闭返回 ERROR_NO_DATA；超时返回 ERROR_TIMEOUT。
std::uint32_t WriteAllOverlapped(HANDLE pipe, std::span<const std::byte> buffer,
                                 std::chrono::milliseconds timeout) noexcept {
    if (buffer.empty()) {
        return ERROR_SUCCESS;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            return ::GetLastError();
        }
        common::UniqueHandle eventGuard(ov.hEvent);

        DWORD bytesWritten = 0;
        const BOOL ok = ::WriteFile(pipe, buffer.data() + offset,
                                    static_cast<DWORD>(buffer.size() - offset),
                                    &bytesWritten, &ov);
        if (!ok) {
            const std::uint32_t error = ::GetLastError();
            if (error != ERROR_IO_PENDING) {
                return error;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || !WaitOverlapped(pipe, ov, remaining)) {
                return ERROR_TIMEOUT;
            }
            if (!::GetOverlappedResult(pipe, &ov, &bytesWritten, FALSE)) {
                return ::GetLastError();
            }
        }
        if (bytesWritten == 0) {
            return ERROR_NO_DATA; // 对端已关闭
        }
        offset += bytesWritten;
    }
    return ERROR_SUCCESS;
}

// 只读查询进程的用户 SID（IPC-006，访问令牌）：OpenProcess(PROCESS_QUERY_INFORMATION)
// -> OpenProcessToken(TOKEN_QUERY) -> GetTokenInformation(TokenUser) ->
// ConvertSidToStringSidW。同用户进程可读（无提权）；跨用户/受保护进程失败返回空串
//（未知身份）。不请求任何额外权限。
std::wstring QueryClientUserSid(std::uint32_t pid) noexcept {
    if (pid == 0) {
        return {};
    }
    common::UniqueHandle process(
        ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!process.IsValid()) {
        return {};
    }
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(process.Get(), TOKEN_QUERY, &rawToken)) {
        return {};
    }
    common::UniqueHandle token(rawToken);

    DWORD needed = 0;
    (void)::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        return {};
    }
    std::vector<std::byte> buffer(needed);
    if (!::GetTokenInformation(token.Get(), TokenUser, buffer.data(), needed,
                               &needed)) {
        return {};
    }
    const auto& tokenUser =
        *reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (tokenUser.User.Sid == nullptr) {
        return {};
    }
    LPWSTR sidText = nullptr;
    if (!::ConvertSidToStringSidW(tokenUser.User.Sid, &sidText) ||
        sidText == nullptr) {
        return {};
    }
    std::unique_ptr<void, common::LocalFreeDeleter> sidGuard(sidText);
    return sidText;
}

// 真实 Win32 服务端后端：CreateNamedPipeW（显式 SDDL + 重叠 I/O）。
class Win32IpcServerBackend final : public IpcServerBackend {
public:
    explicit Win32IpcServerBackend(std::wstring pipeName)
        : pipeName_(std::move(pipeName)) {}

    common::Result<void> CreateAndListen() noexcept override {
        // 幂等：持续受理（SVC-003）下实例已存在则复用，避免关闭重建造成监听空窗。
        if (pipe_.IsValid()) {
            return common::Result<void>::Success();
        }
        Close(); // 防御：清理上次残留

        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(),
                "ConvertStringSecurityDescriptorToSecurityDescriptorW"));
        }
        std::unique_ptr<void, common::LocalFreeDeleter> descriptorGuard(
            descriptor);
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = descriptor;
        sa.bInheritHandle = FALSE;

        const HANDLE pipe = ::CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kMaxPayloadLength, // 输出缓冲区：帧级足够
            kMaxPayloadLength, // 输入缓冲区
            0,                 // 默认客户端连接超时
            &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(), "CreateNamedPipeW"));
        }
        pipe_.Reset(pipe);
        clientPid_ = 0;
        clientSessionId_ = 0;
        clientUserSid_.clear();
        return common::Result<void>::Success();
    }

    common::Result<void> AcceptClient(
        std::chrono::milliseconds timeout) noexcept override {
        if (!pipe_.IsValid()) {
            return common::Result<void>::Failure(common::Error::Validation(
                "AcceptClient", L"管道未创建"));
        }
        clientPid_ = 0;
        clientSessionId_ = 0;
        clientUserSid_.clear();

        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(), "CreateEventW"));
        }
        common::UniqueHandle eventGuard(ov.hEvent);

        const BOOL connected = ::ConnectNamedPipe(pipe_.Get(), &ov);
        if (!connected) {
            const std::uint32_t error = ::GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                // 客户端在监听后立即连接：视为已接受。
            } else if (error == ERROR_IO_PENDING) {
                const DWORD waitResult = ::WaitForSingleObject(
                    ov.hEvent, static_cast<DWORD>(timeout.count()));
                if (waitResult == WAIT_TIMEOUT) {
                    ::CancelIoEx(pipe_.Get(), &ov);
                    ::WaitForSingleObject(ov.hEvent, INFINITE);
                    return common::Result<void>::Failure(common::Error::FromWin32(
                        ERROR_TIMEOUT, "ConnectNamedPipe"));
                }
                if (waitResult != WAIT_OBJECT_0) {
                    ::CancelIoEx(pipe_.Get(), &ov);
                    ::WaitForSingleObject(ov.hEvent, INFINITE);
                    return common::Result<void>::Failure(common::Error::FromWin32(
                        ::GetLastError(), "ConnectNamedPipe"));
                }
                DWORD ignored = 0;
                if (!::GetOverlappedResult(pipe_.Get(), &ov, &ignored, FALSE)) {
                    return common::Result<void>::Failure(common::Error::FromWin32(
                        ::GetLastError(), "GetOverlappedResult"));
                }
            } else {
                return common::Result<void>::Failure(
                    common::Error::FromWin32(error, "ConnectNamedPipe"));
            }
        }

        // 客户端身份：PID + 会话 + 用户 SID（最小权限只读查询，失败按未知处理）。
        DWORD pid = 0;
        if (::GetNamedPipeClientProcessId(pipe_.Get(), &pid)) {
            clientPid_ = static_cast<std::uint32_t>(pid);
        }
        if (clientPid_ != 0) {
            DWORD session = 0;
            if (::ProcessIdToSessionId(clientPid_, &session)) {
                clientSessionId_ = static_cast<std::uint32_t>(session);
            }
            clientUserSid_ = QueryClientUserSid(clientPid_);
        }
        return common::Result<void>::Success();
    }

    common::Result<void> ReadAll(
        std::span<std::byte> buffer,
        std::chrono::milliseconds timeout) noexcept override {
        if (!pipe_.IsValid()) {
            return common::Result<void>::Failure(common::Error::Validation(
                "ReadAll", L"管道未创建"));
        }
        const std::uint32_t error =
            ReadAllOverlapped(pipe_.Get(), buffer, timeout);
        if (error != ERROR_SUCCESS) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(error, "ReadAll"));
        }
        return common::Result<void>::Success();
    }

    common::Result<void> WriteAll(
        std::span<const std::byte> buffer,
        std::chrono::milliseconds timeout) noexcept override {
        if (!pipe_.IsValid()) {
            return common::Result<void>::Failure(common::Error::Validation(
                "WriteAll", L"管道未创建"));
        }
        const std::uint32_t error =
            WriteAllOverlapped(pipe_.Get(), buffer, timeout);
        if (error != ERROR_SUCCESS) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(error, "WriteAll"));
        }
        return common::Result<void>::Success();
    }

    common::Result<void> DisconnectClient() noexcept override {
        if (!pipe_.IsValid()) {
            return common::Result<void>::Success(); // 未连接/已关闭：幂等
        }
        if (!::DisconnectNamedPipe(pipe_.Get())) {
            const std::uint32_t error = ::GetLastError();
            if (error != ERROR_PIPE_NOT_CONNECTED) {
                return common::Result<void>::Failure(
                    common::Error::FromWin32(error, "DisconnectNamedPipe"));
            }
        }
        clientPid_ = 0;
        clientSessionId_ = 0;
        clientUserSid_.clear();
        return common::Result<void>::Success();
    }

    void Close() noexcept override {
        if (pipe_.IsValid()) {
            ::DisconnectNamedPipe(pipe_.Get()); // 忽略错误
        }
        pipe_.Reset();
        clientPid_ = 0;
        clientSessionId_ = 0;
        clientUserSid_.clear();
    }

    std::uint32_t ClientPid() const noexcept override {
        return clientPid_;
    }

    std::uint32_t ClientSessionId() const noexcept override {
        return clientSessionId_;
    }

    std::wstring ClientUserSid() const override {
        return clientUserSid_;
    }

private:
    std::wstring pipeName_;
    common::UniqueHandle pipe_;
    std::uint32_t clientPid_ = 0;
    std::uint32_t clientSessionId_ = 0;
    std::wstring clientUserSid_;
};

// 真实 Win32 客户端后端：CreateFileW（重叠 I/O）+ 忙等待重试。
class Win32IpcClientBackend final : public IpcClientBackend {
public:
    common::Result<void> Connect(
        std::wstring_view pipePath,
        std::chrono::milliseconds timeout) noexcept override {
        pipe_.Reset();
        const std::wstring path(pipePath);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const HANDLE handle = ::CreateFileW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                pipe_.Reset(handle);
                return common::Result<void>::Success();
            }
            const std::uint32_t error = ::GetLastError();
            if (error != ERROR_PIPE_BUSY) {
                return common::Result<void>::Failure(
                    common::Error::FromWin32(error, "CreateFileW"));
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return common::Result<void>::Failure(common::Error::FromWin32(
                    ERROR_TIMEOUT, "WaitNamedPipeW"));
            }
            if (!::WaitNamedPipeW(path.c_str(),
                                  static_cast<DWORD>(remaining.count()))) {
                const std::uint32_t waitError = ::GetLastError();
                if (waitError == ERROR_SEM_TIMEOUT) {
                    continue; // 剩余时间耗尽前重试
                }
                return common::Result<void>::Failure(
                    common::Error::FromWin32(waitError, "WaitNamedPipeW"));
            }
        }
    }

    common::Result<void> WriteAll(
        std::span<const std::byte> buffer,
        std::chrono::milliseconds timeout) noexcept override {
        if (!pipe_.IsValid()) {
            return common::Result<void>::Failure(common::Error::Validation(
                "WriteAll", L"未连接"));
        }
        const std::uint32_t error =
            WriteAllOverlapped(pipe_.Get(), buffer, timeout);
        if (error != ERROR_SUCCESS) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(error, "WriteAll"));
        }
        return common::Result<void>::Success();
    }

    common::Result<void> ReadAll(
        std::span<std::byte> buffer,
        std::chrono::milliseconds timeout) noexcept override {
        if (!pipe_.IsValid()) {
            return common::Result<void>::Failure(common::Error::Validation(
                "ReadAll", L"未连接"));
        }
        const std::uint32_t error =
            ReadAllOverlapped(pipe_.Get(), buffer, timeout);
        if (error != ERROR_SUCCESS) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(error, "ReadAll"));
        }
        return common::Result<void>::Success();
    }

    void Close() noexcept override {
        pipe_.Reset();
    }

private:
    common::UniqueHandle pipe_;
};

} // namespace

std::shared_ptr<IpcServerBackend> CreateWin32ServerBackend(
    std::wstring pipeName) {
    return std::make_shared<Win32IpcServerBackend>(std::move(pipeName));
}

std::shared_ptr<IpcClientBackend> CreateWin32ClientBackend() {
    return std::make_shared<Win32IpcClientBackend>();
}

} // namespace optimizer::ipc
