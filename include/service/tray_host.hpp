#pragma once

#include "common/error.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace optimizer::service {

// 托盘菜单命令 ID：隐藏窗口 WM_COMMAND 的退出项（右键菜单“退出”或测试投递使用）。
inline constexpr std::uint16_t kTrayExitCommandId = 1;

// 托盘图标后端（可注入 fake 单测）。真实实现经 Shell_NotifyIconW 添加/移除通知区图标；
// fake 记录调用序列以确定性验证生命周期与失败路径。
class TrayIconBackend {
public:
    virtual ~TrayIconBackend() = default;

    // 添加通知区图标（NIM_ADD）。hwnd 为宿主隐藏窗口（消息接收者），tooltip 为悬停提示。
    // 失败返回错误（不添加任何图标状态）。
    [[nodiscard]] virtual common::Result<void> AddIcon(
        void* hwnd, const std::wstring& tooltip) = 0;

    // 移除通知区图标（NIM_DELETE）。幂等、尽力而为（窗口销毁前调用；窗口随进程退出时
    // 系统自动清理通知区图标）。
    virtual void RemoveIcon(void* hwnd) noexcept = 0;
};

// 真实 Win32 托盘图标后端（Shell_NotifyIconW）。
[[nodiscard]] std::shared_ptr<TrayIconBackend> CreateWin32TrayIconBackend();

// 托盘宿主（控制台/托盘形态的托盘侧）：在独立线程创建隐藏窗口 + 通知区图标并跑消息
// 循环；业务负载（tick）由调用方在其它线程/宿主循环运行，本类只负责托盘 UI 生命周期。
// 线程模型：worker_ 为唯一 UI 线程（创建窗口、处理托盘消息、弹出右键菜单）；外部线程只经
// Stop()（幂等）请求退出——置停止标志并向隐藏窗口投递 WM_CLOSE，UI 线程移除图标、销毁
// 窗口、PostQuitMessage 后退出；Stop() join 该线程（勿在 UI 线程自身调用）。
// 托盘菜单“退出”回调 onExit（在 UI 线程执行）：必须快速返回，典型实现为请求业务宿主停止
//（如 ServiceHost::RequestStop），由业务侧随后调用本对象 Stop() 完成收尾。
class TrayHost {
public:
    // 托盘菜单“退出”回调（右键菜单选择退出时在 UI 线程回调一次，不得阻塞）。
    using ExitCallback = std::function<void()>;

    struct Options {
        std::wstring windowClass = L"CppOptimizerTrayWindow";
        std::wstring windowTitle = L"CppOptimizer R0 host";
        std::wstring tooltip = L"CppOptimizer R0 host - right-click to exit";
    };

    enum class State {
        Idle,     // 未启动或已停止
        Starting, // 启动中（窗口/图标创建阶段）
        Running,  // 图标已添加、消息循环运行中
    };

    explicit TrayHost(Options options = {});
    ~TrayHost() noexcept;

    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;

    // 启动托盘线程：创建隐藏窗口 -> 添加图标 -> 进入消息循环；返回前等待图标就绪。
    // 窗口创建或图标添加失败返回错误，不遗留线程/窗口/图标（失败不伪装成功）。
    // 已运行时再次调用报错。backend 为空时使用真实 Win32 后端。
    [[nodiscard]] common::Result<void> Start(
        ExitCallback onExit,
        std::shared_ptr<TrayIconBackend> backend = nullptr) noexcept;

    // 请求停止并等待 UI 线程退出（幂等；未启动/已停止为空操作）。会移除图标并注销窗口
    // 类。不能在 UI 线程自身调用（托盘菜单“退出”路径不调用本方法）。
    void Stop() noexcept;

    [[nodiscard]] State GetState() const noexcept;
    // 图标已添加（Start 成功即 true；Stop/窗口关闭后 false）。
    [[nodiscard]] bool IsIconAdded() const noexcept;
    // 隐藏窗口句柄（Running 期间有效；诊断/测试投递消息用）。未运行返回 nullptr。
    [[nodiscard]] void* WindowHandle() const noexcept;

private:
    void Worker(ExitCallback onExit,
                std::shared_ptr<TrayIconBackend> backend) noexcept;
    // 隐藏窗口过程（静态成员：经 GWLP_USERDATA 取实例，可访问私有方法）。
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam,
                                       LPARAM lParam) noexcept;
    // UI 线程：移除图标并销毁窗口（WM_CLOSE 处理）。
    void CloseFromUiThread() noexcept;
    // 外部线程请求退出：置停止标志并向隐藏窗口投递 WM_CLOSE（窗口未建好时由
    // Worker 发布成功后自检退出）。
    void RequestStopFromAnyThread() noexcept;
    // 托盘菜单“退出”（UI 线程；onExit 至多回调一次）。
    void RequestExitFromUiThread() noexcept;
    // 右键弹出菜单并处理选中项（UI 线程，WM_APP+1 回调）。
    void ShowTrayMenu(HWND hwnd) noexcept;

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    State state_ = State::Idle;
    void* hwnd_ = nullptr;               // 隐藏窗口句柄（UI 线程写，其它线程锁内读）
    bool iconAdded_ = false;             // 图标已添加（UI 线程维护）
    std::atomic<bool> stopRequested_{false}; // 停止请求（跨线程置位）
    std::atomic<bool> exitRequested_{false}; // 菜单“退出”已触发（onExit 至多一次）
    std::optional<common::Error> startError_; // 启动失败原因（Worker 发布）
    ExitCallback onExit_;                // 菜单退出回调（仅 UI 线程使用）
    std::shared_ptr<TrayIconBackend> backend_; // 图标后端（Worker 持有）
    std::thread worker_;
};

} // namespace optimizer::service
