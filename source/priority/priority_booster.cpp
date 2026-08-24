#include "priority/priority_booster.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

namespace optimizer::priority {

namespace {

// 打开目标进程的最小权限集：查询有限信息（创建时间/优先级类）、
// 设置优先级类、等待进程退出句柄。不申请更大权限（黄色不变量）。
constexpr DWORD kMinProcessAccess = PROCESS_QUERY_LIMITED_INFORMATION |
                                    PROCESS_SET_INFORMATION | SYNCHRONIZE;

// 等级上限校验：level 高于 maxLevel 返回 true。枚举顺序 None < AboveNormal < High。
bool LevelExceeds(config::PriorityLevel level,
                  config::PriorityLevel maxLevel) noexcept {
    return static_cast<int>(level) > static_cast<int>(maxLevel);
}

} // namespace

common::Result<std::uint32_t> PriorityLevelToWin32Class(
    config::PriorityLevel level) noexcept {
    switch (level) {
        case config::PriorityLevel::AboveNormal:
            return common::Result<std::uint32_t>::Success(
                0x00008000u); // ABOVE_NORMAL_PRIORITY_CLASS
        case config::PriorityLevel::High:
            return common::Result<std::uint32_t>::Success(
                0x00000080u); // HIGH_PRIORITY_CLASS
        case config::PriorityLevel::None:
            break;
    }
    return common::Result<std::uint32_t>::Failure(common::Error::Validation(
        "PriorityLevelToWin32Class", L"no boost level (none)"));
}

common::Result<std::uint64_t> Win32PriorityBackend::OpenProcess(
    std::uint32_t pid) {
    HANDLE handle = ::OpenProcess(kMinProcessAccess, FALSE, pid);
    if (handle == nullptr) {
        return common::Result<std::uint64_t>::Failure(
            common::Error::FromWin32(::GetLastError(), "OpenProcess"));
    }
    return common::Result<std::uint64_t>::Success(
        reinterpret_cast<std::uint64_t>(handle));
}

std::uint64_t Win32PriorityBackend::QueryCreationTime(
    std::uint64_t handle) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!::GetProcessTimes(reinterpret_cast<HANDLE>(handle), &creation,
                           &exit, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
           creation.dwLowDateTime;
}

std::uint32_t Win32PriorityBackend::QueryPriorityClass(
    std::uint64_t handle) {
    // GetPriorityClass 失败返回 0；0 不是任何合法优先级类常量，调用方按失败处理。
    return ::GetPriorityClass(reinterpret_cast<HANDLE>(handle));
}

common::Result<void> Win32PriorityBackend::SetPriorityClass(
    std::uint64_t handle, std::uint32_t priorityClass) {
    if (!::SetPriorityClass(reinterpret_cast<HANDLE>(handle),
                            priorityClass)) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "SetPriorityClass"));
    }
    return common::Result<void>::Success();
}

void Win32PriorityBackend::CloseProcess(std::uint64_t handle) noexcept {
    if (handle != 0) {
        // 尽力而为：关闭失败不抛出；进程退出时句柄随句柄表关闭。
        (void)::CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
}

std::shared_ptr<PriorityBackend> CreateWin32Backend() noexcept {
    return std::make_shared<Win32PriorityBackend>();
}

PriorityBooster::PriorityBooster(std::shared_ptr<PriorityBackend> backend,
                                 Options options) noexcept
    : backend_(std::move(backend)), options_(options) {}

PriorityBooster::~PriorityBooster() noexcept {
    ReleaseAll();
}

common::Result<void> PriorityBooster::AcquireBoost(
    std::string gameId, std::uint32_t pid,
    std::uint64_t expectedCreationTime100ns, config::PriorityLevel level) {
    if (level == config::PriorityLevel::None) {
        return common::Result<void>::Failure(common::Error::Validation(
            "PriorityBooster::AcquireBoost", L"no boost level (none)"));
    }
    if (LevelExceeds(level, options_.maxLevel)) {
        return common::Result<void>::Failure(common::Error::Validation(
            "PriorityBooster::AcquireBoost",
            L"priority level exceeds configured maximum"));
    }

    auto it = leases_.find(gameId);
    if (it != leases_.end() && it->second.pid == pid) {
        // 同游戏同 pid：多租约，递增计数，不重复打开/设置。
        ++it->second.count;
        return common::Result<void>::Success();
    }
    if (it != leases_.end()) {
        // 同游戏不同 pid（重启）：先按最后租约条件释放旧租约；释放失败
        // 说明旧提升未能恢复，不应继续对旧进程之外的新进程叠加，向上报错。
        const auto released =
            ReleaseBoost(gameId, it->second.pid);
        if (!released) {
            return released;
        }
    }

    const auto target = PriorityLevelToWin32Class(level);
    if (!target) {
        return common::Result<void>::Failure(target.ErrorValue());
    }

    // 全新获取：打开 -> 身份校验 -> 保存原值 -> 设置目标值；任一失败不改状态。
    auto opened = backend_->OpenProcess(pid);
    if (!opened) {
        return common::Result<void>::Failure(opened.ErrorValue());
    }
    const std::uint64_t handle = opened.Value();

    const std::uint64_t actualCreationTime =
        backend_->QueryCreationTime(handle);
    if (expectedCreationTime100ns != 0 &&
        (actualCreationTime == 0 ||
         actualCreationTime != expectedCreationTime100ns)) {
        // 观察与打开之间 PID 被重用（或无法验证身份）：拒绝，绝不作用于新进程。
        backend_->CloseProcess(handle);
        return common::Result<void>::Failure(common::Error::Validation(
            "PriorityBooster::AcquireBoost",
            L"process identity mismatch (pid reused or unverifiable)"));
    }

    const std::uint32_t originalClass =
        backend_->QueryPriorityClass(handle);
    if (originalClass == 0) {
        backend_->CloseProcess(handle); // 无法保存原值，不提升
        return common::Result<void>::Failure(common::Error{
            common::ErrorDomain::Internal, 0,
            "PriorityBooster::AcquireBoost",
            L"failed to query current priority class"});
    }

    auto set = backend_->SetPriorityClass(handle, target.Value());
    if (!set) {
        backend_->CloseProcess(handle); // 不留悬空句柄
        return common::Result<void>::Failure(set.ErrorValue());
    }

    LeaseState state;
    state.pid = pid;
    state.creationTime100ns =
        actualCreationTime != 0 ? actualCreationTime : expectedCreationTime100ns;
    state.handle = handle;
    state.originalClass = originalClass;
    state.targetClass = target.Value();
    state.level = level;
    state.count = 1;
    leases_.insert_or_assign(std::move(gameId), std::move(state));
    return common::Result<void>::Success();
}

common::Result<void> PriorityBooster::ReleaseBoost(std::string_view gameId,
                                                   std::uint32_t pid) {
    auto it = leases_.find(gameId);
    if (it == leases_.end()) {
        return common::Result<void>::Success(); // 幂等：未持有为空操作
    }
    LeaseState& state = it->second;
    if (state.pid != pid) {
        // 释放目标与租约记录不一致：可能 PID 混淆，拒绝且不动状态。
        return common::Result<void>::Failure(common::Error::Validation(
            "PriorityBooster::ReleaseBoost",
            L"release pid does not match the boosted process"));
    }
    if (state.count > 1) {
        --state.count;
        return common::Result<void>::Success();
    }
    common::Error restoreError;
    if (!RestoreLastLease(state, restoreError)) {
        return common::Result<void>::Failure(std::move(restoreError));
    }
    leases_.erase(it);
    return common::Result<void>::Success();
}

bool PriorityBooster::RestoreLastLease(LeaseState& state,
                                       common::Error& errorOut) {
    // 恢复前重验身份：创建时间变化视为进程被回收，放弃恢复（防御性；打开句柄
    // 期间进程对象被固定，正常路径不会出现）。
    const std::uint64_t currentCreationTime =
        backend_->QueryCreationTime(state.handle);
    if (state.creationTime100ns != 0 &&
        currentCreationTime != state.creationTime100ns) {
        backend_->CloseProcess(state.handle);
        state.handle = 0;
        state.count = 0;
        return true;
    }

    const std::uint32_t currentClass =
        backend_->QueryPriorityClass(state.handle);
    if (currentClass == 0) {
        // 进程已退出/不可查询：优先级随进程消失，无需恢复（正常取消）。
        backend_->CloseProcess(state.handle);
        state.handle = 0;
        state.count = 0;
        return true;
    }
    if (currentClass != state.targetClass) {
        // 当前值被外部改动：不覆盖用户/第三方工具的修改，仅清理本模块租约。
        backend_->CloseProcess(state.handle);
        state.handle = 0;
        state.count = 0;
        return true;
    }

    auto restored =
        backend_->SetPriorityClass(state.handle, state.originalClass);
    if (!restored) {
        // 恢复失败：提升仍生效，恢复计数与句柄，调用方可重试（不谎报已恢复）。
        state.count = 1;
        errorOut = restored.ErrorValue();
        return false;
    }
    backend_->CloseProcess(state.handle);
    state.handle = 0;
    state.count = 0;
    return true;
}

void PriorityBooster::ForceCleanLease(LeaseState& state) noexcept {
    if (state.handle != 0 && backend_) {
        const std::uint32_t currentClass =
            backend_->QueryPriorityClass(state.handle);
        if (currentClass != 0 && currentClass == state.targetClass) {
            // 尽力而为恢复原值；失败不阻断清理（句柄关闭后进程对象随退出释放）。
            (void)backend_->SetPriorityClass(state.handle,
                                             state.originalClass);
        }
    }
    if (state.handle != 0 && backend_) {
        backend_->CloseProcess(state.handle);
    }
    state.handle = 0;
    state.count = 0;
}

void PriorityBooster::ReleaseAll() noexcept {
    for (auto& [gameId, state] : leases_) {
        (void)gameId;
        ForceCleanLease(state);
    }
    leases_.clear();
}

bool PriorityBooster::IsBoosted(std::string_view gameId) const noexcept {
    const auto it = leases_.find(gameId);
    return it != leases_.end() && it->second.count > 0;
}

std::size_t PriorityBooster::BoostCount(
    std::string_view gameId) const noexcept {
    const auto it = leases_.find(gameId);
    return it == leases_.end() ? 0 : it->second.count;
}

std::vector<PriorityBooster::BoostState> PriorityBooster::GetStates()
    const noexcept {
    std::vector<BoostState> states;
    states.reserve(leases_.size());
    for (const auto& [gameId, state] : leases_) {
        BoostState out;
        out.gameId = gameId;
        out.pid = state.pid;
        out.level = state.level;
        out.count = state.count;
        out.creationTime100ns = state.creationTime100ns;
        states.push_back(std::move(out));
    }
    return states;
}

} // namespace optimizer::priority
