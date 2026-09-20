#pragma once

#include "common/error.hpp"

#include <memory>
#include <string>

namespace optimizer::service {

// 自启动项（每用户）：在 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 下维护一个值。
// 契约：
// - **仅当前用户**、**标准用户即可**（无需提权），不触碰其它值与他人的配置；
// - 可逆：Remove 即恢复原状（存在则删、不存在视为成功——幂等）；
// - 失败如实返回 Win32 错误，不伪装成功；查询未注册返回空串（Success，不是错误）；
// - **默认不自动安装**：只有显式调用 Install 才写入（保守默认，见危险操作策略）。
// 线程要求：可在任意线程调用（不持有跨调用锁；注册表 API 自身线程安全）。
inline constexpr const wchar_t* kStartupValueName = L"CppOptimizer";

// 注册自启动：写入 "<exePath>"（带引号以容忍路径空格）。空路径拒绝。
[[nodiscard]] common::Result<void> InstallStartupEntry(const std::wstring& exePath) noexcept;

// 注销自启动：删除该值（不存在视为成功）。不触碰其它值。
[[nodiscard]] common::Result<void> RemoveStartupEntry() noexcept;

// 查询已注册的命令行；未注册返回空串。
[[nodiscard]] common::Result<std::wstring> QueryStartupEntry() noexcept;

// 可注入后端：把“读/写/删自启动项”抽成接口，使单测可用 fake 而不触碰真实注册表
// （项目测试原则：单元测试不调用真实系统副作用）。
// 契约：Read 未注册返回空串（不是错误）；Write 收到**已带引号**的命令行；
// Remove 删除不存在视为成功（幂等）；任何失败如实返回错误域（不得伪成功）。
class StartupEntryBackend {
public:
    virtual ~StartupEntryBackend() = default;
    [[nodiscard]] virtual common::Result<std::wstring> Read() = 0;
    [[nodiscard]] virtual common::Result<void> Write(
        const std::wstring& quotedCommand) = 0;
    [[nodiscard]] virtual common::Result<void> Remove() = 0;
};

// 面向后端的操作（纯逻辑：空路径拒绝 + 带引号构造）。
[[nodiscard]] common::Result<void> InstallStartupEntry(
    StartupEntryBackend& backend, const std::wstring& exePath) noexcept;
[[nodiscard]] common::Result<void> RemoveStartupEntry(
    StartupEntryBackend& backend) noexcept;
[[nodiscard]] common::Result<std::wstring> QueryStartupEntry(
    StartupEntryBackend& backend) noexcept;

// Win32 实现（HKCU Run）；分配失败返回 nullptr。
[[nodiscard]] std::shared_ptr<StartupEntryBackend>
CreateWin32StartupEntryBackend() noexcept;

} // namespace optimizer::service
