#include "service/host_instance.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>
#include <vector>

namespace optimizer::service {

namespace {

// 命名互斥量的 RAII：句柄关闭即释放独占（进程退出由系统回收）。
class Win32HostInstanceLock final : public HostInstanceLock {
public:
    explicit Win32HostInstanceLock(HANDLE handle) noexcept : handle_(handle) {}
    ~Win32HostInstanceLock() noexcept override {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }
    Win32HostInstanceLock(const Win32HostInstanceLock&) = delete;
    Win32HostInstanceLock& operator=(const Win32HostInstanceLock&) = delete;

    [[nodiscard]] bool IsHeld() const noexcept override {
        return handle_ != nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

} // namespace

std::shared_ptr<HostInstanceLock> TryAcquireHostInstance(
    const std::wstring& name) noexcept {
    if (name.empty()) {
        return nullptr; // 空名称：拒绝（无名互斥量无法表达“同一实例”）
    }
    // CreateMutexW 成功但 GetLastError 报 ERROR_ALREADY_EXISTS = 已有实例持有同名互斥量。
    // 注意：此时句柄仍打开，必须关闭以免泄漏（旧实例退出前它一直存在）。
    HANDLE handle = ::CreateMutexW(nullptr, TRUE, name.c_str());
    if (handle == nullptr) {
        return nullptr; // 创建失败（权限/名称非法）：按“无法独占”如实处理
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(handle);
        return nullptr;
    }
    try {
        return std::make_shared<Win32HostInstanceLock>(handle);
    } catch (...) {
        ::CloseHandle(handle); // 分配失败：如实释放，不泄漏句柄
        return nullptr;
    }
}

std::wstring DefaultHostInstanceName() noexcept {
    // Local\ 前缀 = 当前会话命名空间；再拼用户名，使同一用户的多个会话也互斥
    // （每用户文件是共享的，跨会话同样会争抢）。
    std::wstring name = L"Local\\CppOptimizerHost";
    // 两次查询模式：`pcbBuffer` 是**输入输出**参数——输入必须是缓冲区容量，否则调用失败且
    // 容量不被更新，循环无法收敛。此处还保证每次迭代容量必然增长，并设上限防无界增长。
    std::vector<wchar_t> buffer(64);
    for (;;) {
        DWORD written = static_cast<DWORD>(buffer.size());
        if (::GetUserNameW(buffer.data(), &written) != 0) {
            name += L"_";
            name.append(buffer.data(), written > 0 ? written - 1 : 0);
            return name;
        }
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return name; // 其它失败（如无令牌）：退回会话级名称，不阻断启动
        }
        const std::size_t required =
            written > buffer.size() ? written : buffer.size() * 2;
        if (required > 1024) {
            return name; // 用户名异常长：不无界增长，退回会话级名称
        }
        buffer.resize(required);
    }
}

} // namespace optimizer::service
