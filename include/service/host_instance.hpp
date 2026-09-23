#pragma once

#include <memory>
#include <string>

namespace optimizer::service {

// 宿主单实例守卫：同一用户会话内只允许一个常驻宿主受理 Agent。
// 存在的理由：自启动项 / 计划任务 / SCM 服务三种形态可被同时注册，若无守卫，
// 登录后会拉起多个宿主——它们会争抢同一个命名管道名与同一批每用户文件
// （config / recovery-state.json / presence-timeline.log / audit.log），
// 造成事实分散与恢复标记互相清除（出现假的“上次异常退出未确认”）。
//
// 契约：
// - **按用户**命名（同一用户跨会话也只允许一个宿主）：名称含当前用户名；
// - 获取失败（已有实例）返回 false，**不阻塞、不重试**（由调用方如实提示并退出）；
// - 生命周期与对象绑定：对象析构即释放锁（进程退出由系统回收，不依赖显式清理）；
// - 空名称拒绝（返回 nullptr，不生成无名互斥量）。
class HostInstanceLock {
public:
    virtual ~HostInstanceLock() = default;

    // 是否已成功独占（实现恒返回 true；失败实例由工厂返回 nullptr）。
    [[nodiscard]] virtual bool IsHeld() const noexcept = 0;
};

// 尝试获取当前用户的宿主实例锁；已被占用返回 nullptr（调用方据此如实提示并退出）。
[[nodiscard]] std::shared_ptr<HostInstanceLock> TryAcquireHostInstance(
    const std::wstring& name) noexcept;

// 默认锁名（纯字符串构造，不访问系统）：Local\CppOptimizerHost_<用户名>。
// 取不到用户名时退化为 Local\CppOptimizerHost（仍为会话级隔离）。
[[nodiscard]] std::wstring DefaultHostInstanceName() noexcept;

} // namespace optimizer::service
