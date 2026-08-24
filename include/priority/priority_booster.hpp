#pragma once

#include "common/error.hpp"
#include "config/config_manager.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::priority {

// 纯函数：配置优先级等级 -> Win32 优先级类常量。
// None 返回 Validation 错误（不提升）；realtime 由配置层在枚举层拒绝（无枚举值）。
[[nodiscard]] common::Result<std::uint32_t> PriorityLevelToWin32Class(
    config::PriorityLevel level) noexcept;

// 进程优先级后端（可注入 fake 以便确定性测试；黄色不变量见 PriorityBooster 契约）。
// handle 为不透明句柄 id，0 表示无效/未打开。
class PriorityBackend {
public:
    virtual ~PriorityBackend() = default;

    // 打开目标进程（最小权限 PROCESS_QUERY_LIMITED_INFORMATION |
    // PROCESS_SET_INFORMATION | SYNCHRONIZE）。失败返回 common::Error，不抛出。
    [[nodiscard]] virtual common::Result<std::uint64_t> OpenProcess(
        std::uint32_t pid) = 0;

    // 查询进程创建时间（100ns FILETIME 单位）。失败返回 0，按"未知"处理。
    [[nodiscard]] virtual std::uint64_t QueryCreationTime(
        std::uint64_t handle) = 0;

    // 查询当前优先级类（Win32 优先级类常量）。失败返回 0（0 不是合法常量）。
    [[nodiscard]] virtual std::uint32_t QueryPriorityClass(
        std::uint64_t handle) = 0;

    // 设置优先级类。失败返回 common::Error。
    [[nodiscard]] virtual common::Result<void> SetPriorityClass(
        std::uint64_t handle, std::uint32_t priorityClass) = 0;

    // 关闭句柄（幂等，尽力而为，不抛出）。
    virtual void CloseProcess(std::uint64_t handle) noexcept = 0;
};

// Win32 实现：OpenProcess / GetProcessTimes / GetPriorityClass /
// SetPriorityClass / CloseHandle（kernel32，无新增链接库）。
class Win32PriorityBackend final : public PriorityBackend {
public:
    [[nodiscard]] common::Result<std::uint64_t> OpenProcess(
        std::uint32_t pid) override;
    [[nodiscard]] std::uint64_t QueryCreationTime(
        std::uint64_t handle) override;
    [[nodiscard]] std::uint32_t QueryPriorityClass(
        std::uint64_t handle) override;
    [[nodiscard]] common::Result<void> SetPriorityClass(
        std::uint64_t handle, std::uint32_t priorityClass) override;
    void CloseProcess(std::uint64_t handle) noexcept override;
};

// 创建 Win32 后端（恒成功，返回非空）。
[[nodiscard]] std::shared_ptr<PriorityBackend> CreateWin32Backend() noexcept;

// 优先级提升器（按 gameId 的租约状态机，条件恢复）。
// 契约（黄色不变量）：
// - 租约配对：每次成功 AcquireBoost 必须由一次 ReleaseBoost / ReleaseAll 配对；
// - 最小权限：OpenProcess 只请求 PROCESS_QUERY_LIMITED_INFORMATION |
//   PROCESS_SET_INFORMATION | SYNCHRONIZE；
// - 身份重验：AcquireBoost 的 expectedCreationTime100ns 非零且与实测创建时间
//   不一致（或实测未知）时拒绝，防止观察与打开之间 PID 被重用；
// - 条件恢复：最后租约释放时，仅当进程创建时间未变且当前优先级类仍等于
//   模块设置值时才恢复原值；当前值被外部改动时跳过恢复（不覆盖外部修改）；
// - 失败不伪装成功：open/set 失败不改动状态、不留悬空句柄；恢复失败恢复
//   计数与句柄（提升仍生效），调用方可重试；
// - 目标退出视为正常取消：恢复时进程已退出/不可查询则跳过恢复并清理状态。
class PriorityBooster {
public:
    // 允许的最高等级。默认 AboveNormal（High 仅显式配置为高时允许）。
    struct Options {
        config::PriorityLevel maxLevel = config::PriorityLevel::AboveNormal;
    };

    explicit PriorityBooster(std::shared_ptr<PriorityBackend> backend,
                             Options options = {}) noexcept;
    ~PriorityBooster() noexcept;

    PriorityBooster(const PriorityBooster&) = delete;
    PriorityBooster& operator=(const PriorityBooster&) = delete;

    // 获取提升租约。level 必须非 None 且不高于 options.maxLevel；
    // expectedCreationTime100ns 为调用方先前观测的创建时间（无观测传 0）。
    // 同 gameId 同 pid 重复获取递增计数（不再重复打开/设置）；
    // 同 gameId 不同 pid（进程重启）先按最后租约条件释放旧租约再全新获取。
    // 失败时状态不变。
    [[nodiscard]] common::Result<void> AcquireBoost(
        std::string gameId, std::uint32_t pid,
        std::uint64_t expectedCreationTime100ns,
        config::PriorityLevel level);

    // 释放一次租约。pid 必须与记录一致（不一致返回 Validation 错误，状态不变）；
    // 未持有为幂等成功。最后租约释放时执行条件恢复。
    [[nodiscard]] common::Result<void> ReleaseBoost(
        std::string_view gameId, std::uint32_t pid);

    // 释放全部租约（尽力而为的条件恢复 + 关闭句柄）。恒成功。
    void ReleaseAll() noexcept;

    // 是否持有（count > 0）。
    [[nodiscard]] bool IsBoosted(std::string_view gameId) const noexcept;

    // 当前租约计数（0 表示未持有）。
    [[nodiscard]] std::size_t BoostCount(
        std::string_view gameId) const noexcept;

    // 当前持有租约快照（只读，供 CLI 输出与测试）。
    struct BoostState {
        std::string gameId;
        std::uint32_t pid = 0;
        config::PriorityLevel level = config::PriorityLevel::None;
        std::size_t count = 0;
        std::uint64_t creationTime100ns = 0;
    };
    [[nodiscard]] std::vector<BoostState> GetStates() const noexcept;

private:
    struct LeaseState {
        std::uint32_t pid = 0;
        std::uint64_t creationTime100ns = 0;
        std::uint64_t handle = 0;
        std::uint32_t originalClass = 0;
        std::uint32_t targetClass = 0;
        config::PriorityLevel level = config::PriorityLevel::None;
        std::size_t count = 0;
    };

    // 最后租约的条件恢复。返回 true 表示可清理状态（成功恢复或跳过恢复：
    // 目标退出/外部改动/身份不符）；返回 false 表示恢复失败（计数已恢复为 1，
    // 错误写入 errorOut，调用方可重试释放）。
    bool RestoreLastLease(LeaseState& state, common::Error& errorOut);

    // 尽力而为清理（ReleaseAll 用）：忽略恢复失败，始终关闭句柄并清空状态。
    void ForceCleanLease(LeaseState& state) noexcept;

    std::shared_ptr<PriorityBackend> backend_;
    Options options_;
    // 透明比较器（std::less<>）：find 可用 string_view 异构查找，避免为查询分配 string。
    std::map<std::string, LeaseState, std::less<>> leases_;
};

} // namespace optimizer::priority
