#pragma once

#include "common/error.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace optimizer::power {

// 电源请求类型。对应 Windows Power Request 的两类请求：
// - ExecutionRequired：告知系统"正在执行重要任务，不要睡眠"；
// - DisplayRequired：告知系统"需要显示器保持开启，不要熄屏"。
// Power Request 表达的是睡眠/显示需求，不是 CPU/GPU 频率锁
// （红色禁止区：把 Power Request 宣称为锁频）。
enum class PowerLockType {
    ExecutionRequired,
    DisplayRequired
};

// 类型名（纯查询，恒成功）。值：execution / display。
[[nodiscard]] const wchar_t* PowerLockTypeToString(PowerLockType type) noexcept;

// 纯校验：类型名 -> 枚举（execution/display，ASCII 大小写不敏感）。
// "both" 是 CLI 层的多类型概念，不在此枚举内。
[[nodiscard]] common::Result<PowerLockType> ParsePowerLockType(
    std::string_view name) noexcept;

// 电源请求后端（可注入 fake 以便确定性测试；黄色不变量见 PowerLocker 契约）。
// handle 为不透明句柄 id，0 表示无效/未创建。
class PowerRequestBackend {
public:
    virtual ~PowerRequestBackend() = default;

    // 创建电源请求对象（携带 reason 说明，Windows 电源设置界面可见）。
    // 失败返回 common::Error，不抛出。
    [[nodiscard]] virtual common::Result<std::uint64_t> CreateRequest(
        std::wstring_view reason) = 0;

    // 使请求生效（设置指定类型）。
    [[nodiscard]] virtual common::Result<void> SetRequest(
        std::uint64_t handle, PowerLockType type) = 0;

    // 使请求失效（清除指定类型）。
    [[nodiscard]] virtual common::Result<void> ClearRequest(
        std::uint64_t handle, PowerLockType type) = 0;

    // 关闭请求对象（幂等；关闭即取消该句柄上的全部请求，系统侧保证释放）。
    // 尽力而为，不抛出。
    virtual void CloseRequest(std::uint64_t handle) noexcept = 0;
};

// Win32 实现：PowerCreateRequest / PowerSetRequest / PowerClearRequest / CloseHandle。
class Win32PowerRequestBackend final : public PowerRequestBackend {
public:
    [[nodiscard]] common::Result<std::uint64_t> CreateRequest(
        std::wstring_view reason) override;
    [[nodiscard]] common::Result<void> SetRequest(
        std::uint64_t handle, PowerLockType type) override;
    [[nodiscard]] common::Result<void> ClearRequest(
        std::uint64_t handle, PowerLockType type) override;
    void CloseRequest(std::uint64_t handle) noexcept override;
};

// 创建 Win32 后端（恒成功，返回非空）。
[[nodiscard]] std::shared_ptr<PowerRequestBackend> CreateWin32Backend() noexcept;

// 电源锁定器（每类型引用计数状态机）。
// 契约（黄色不变量）：
// - 配对释放：每次成功 AcquireLock 必须由一次 ReleaseLock / ReleaseAll 配对；
// - 引用计数：同类型重复 Acquire 递增计数；仅 0->1 时创建并设置请求，
//   1->0 时清除并关闭；计数归零前请求保持生效；
// - 幂等：对未持有类型的 ReleaseLock 为空操作成功（不改变状态）；
// - 失败不伪装成功：0->1 创建/设置失败不改动计数、不留悬空句柄；
//   1->0 清除失败保留句柄与计数（请求仍生效），调用方可重试释放；
// - 析构自动 ReleaseAll；进程退出时句柄随进程句柄表关闭，系统侧请求自动取消。
class PowerLocker {
public:
    explicit PowerLocker(std::shared_ptr<PowerRequestBackend> backend) noexcept;
    ~PowerLocker() noexcept;

    PowerLocker(const PowerLocker&) = delete;
    PowerLocker& operator=(const PowerLocker&) = delete;

    // 获取锁。reason 在 0->1 创建请求时写入请求对象
    // （Windows 电源设置界面可见）；重复获取不改变已创建请求的 reason。
    // 失败时状态不变（不计数、不留句柄）。
    [[nodiscard]] common::Result<void> AcquireLock(
        PowerLockType type, std::wstring reason);

    // 释放一次。未持有类型为空操作成功（幂等）。
    [[nodiscard]] common::Result<void> ReleaseLock(PowerLockType type);

    // 释放全部类型的全部计数。尽力而为恒成功：
    // 关闭句柄即取消该句柄上的全部系统侧请求。
    void ReleaseAll() noexcept;

    // 是否持有（计数 > 0）。
    [[nodiscard]] bool IsLocked(PowerLockType type) const noexcept;

    // 当前计数（0 表示未持有）。
    [[nodiscard]] std::size_t LockCount(PowerLockType type) const noexcept;

    // 最近一次 0->1 创建时的 reason（未持有时为空）。
    [[nodiscard]] const std::wstring& LockReason(
        PowerLockType type) const noexcept;

private:
    struct LockState {
        std::size_t count = 0;
        std::uint64_t handle = 0;
        std::wstring reason;
    };

    std::shared_ptr<PowerRequestBackend> backend_;
    std::array<LockState, 2> states_;
};

} // namespace optimizer::power
