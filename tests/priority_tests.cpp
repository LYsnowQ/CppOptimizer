#include "priority/priority_booster.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::Result;
using optimizer::config::PriorityLevel;
using optimizer::priority::CreateWin32Backend;
using optimizer::priority::PriorityBackend;
using optimizer::priority::PriorityBooster;
using optimizer::priority::PriorityLevelToWin32Class;

// 记录调用序列的 fake 后端：验证最小权限打开/身份重验/条件恢复/失败路径，
// 不触碰 Win32。句柄 id 从 1000 起，便于断言无悬空句柄。
class FakeBackend final : public PriorityBackend {
public:
    static constexpr std::uint32_t kNormalClass = 0x00000020u; // NORMAL_PRIORITY_CLASS
    static constexpr std::uint32_t kAboveNormalClass = 0x00008000u;

    struct Call {
        std::string op; // open / qct / qcls / set / close
        std::uint64_t handle = 0;
        std::uint32_t pid = 0;
        std::uint32_t value = 0;
    };

    bool failNextOpen = false;
    bool failNextQueryClass = false;   // 一次查询失败（模拟进程退出/权限丢失）
    bool failNextQueryCreation = false;
    bool failNextSet = false;          // 一次设置失败（acquire 或 restore）

    std::uint64_t nextHandle = 1000;
    std::vector<Call> calls;
    std::set<std::uint64_t> open;                 // 仍打开的句柄
    std::map<std::uint32_t, std::uint64_t> handleByPid;
    std::map<std::uint64_t, std::uint32_t> priorityByHandle;
    std::map<std::uint64_t, std::uint64_t> creationByHandle;
    std::map<std::uint32_t, std::uint64_t> creationByPid; // open 时按 pid 初始化

    Result<std::uint64_t> OpenProcess(std::uint32_t pid) override {
        calls.push_back(Call{"open", 0, pid, 0});
        if (failNextOpen) {
            failNextOpen = false;
            return Result<std::uint64_t>::Failure(
                Error::Validation("FakeBackend::OpenProcess",
                                  L"injected open failure"));
        }
        const auto handle = nextHandle++;
        open.insert(handle);
        handleByPid[pid] = handle;
        if (const auto it = creationByPid.find(pid);
            it != creationByPid.end()) {
            creationByHandle[handle] = it->second;
        }
        priorityByHandle[handle] = kNormalClass;
        return Result<std::uint64_t>::Success(handle);
    }

    std::uint64_t QueryCreationTime(std::uint64_t handle) override {
        calls.push_back(Call{"qct", handle, 0, 0});
        if (failNextQueryCreation) {
            failNextQueryCreation = false;
            return 0;
        }
        const auto it = creationByHandle.find(handle);
        return it == creationByHandle.end() ? 0 : it->second;
    }

    std::uint32_t QueryPriorityClass(std::uint64_t handle) override {
        calls.push_back(Call{"qcls", handle, 0, 0});
        if (failNextQueryClass) {
            failNextQueryClass = false;
            return 0;
        }
        const auto it = priorityByHandle.find(handle);
        return it == priorityByHandle.end() ? 0 : it->second;
    }

    Result<void> SetPriorityClass(std::uint64_t handle,
                                  std::uint32_t priorityClass) override {
        calls.push_back(Call{"set", handle, 0, priorityClass});
        if (failNextSet) {
            failNextSet = false;
            return Result<void>::Failure(
                Error::Validation("FakeBackend::SetPriorityClass",
                                  L"injected set failure"));
        }
        priorityByHandle[handle] = priorityClass;
        return Result<void>::Success();
    }

    void CloseProcess(std::uint64_t handle) noexcept override {
        calls.push_back(Call{"close", handle, 0, 0});
        open.erase(handle);
        // 句柄关闭不改变进程自身状态：优先级类保留（进程仍在运行），
        // 与 Win32 语义一致（优先级随进程存活，不随句柄）。
    }

    // 返回进程当前优先级类（句柄未打开时 0）。
    std::uint32_t CurrentClass(std::uint32_t pid) const {
        const auto hit = handleByPid.find(pid);
        if (hit == handleByPid.end()) {
            return 0;
        }
        const auto pit = priorityByHandle.find(hit->second);
        return pit == priorityByHandle.end() ? 0 : pit->second;
    }
};

// ---------- 纯函数 ----------

bool TestLevelToWin32Class() {
    if (PriorityLevelToWin32Class(PriorityLevel::AboveNormal).Value() !=
        FakeBackend::kAboveNormalClass) {
        return false;
    }
    if (PriorityLevelToWin32Class(PriorityLevel::High).Value() !=
        0x00000080u) { // HIGH_PRIORITY_CLASS
        return false;
    }
    if (PriorityLevelToWin32Class(PriorityLevel::None).HasValue()) {
        return false; // none 不提升
    }
    return true;
}

// ---------- Acquire ----------

bool TestAcquireOpensVerifiesAndBoosts() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (!booster.IsBoosted("g") || booster.BoostCount("g") != 1) {
        return false;
    }
    // 调用序列：open(pid) -> qct(身份校验) -> qcls(保存原值) -> set(目标)。
    if (fake->calls.size() != 4) {
        return false;
    }
    if (fake->calls[0].op != "open" || fake->calls[0].pid != 100) {
        return false;
    }
    if (fake->calls[1].op != "qct") {
        return false;
    }
    if (fake->calls[2].op != "qcls") {
        return false;
    }
    if (fake->calls[3].op != "set" ||
        fake->calls[3].value != FakeBackend::kAboveNormalClass) {
        return false;
    }
    // 原值已保存（NORMAL），句柄保持打开（租约存续期间固定进程对象）。
    if (fake->CurrentClass(100) != FakeBackend::kAboveNormalClass) {
        return false;
    }
    return fake->open.size() == 1;
}

bool TestDoubleAcquireSameGameSingleOpen() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (booster.BoostCount("g") != 2) {
        return false;
    }
    std::size_t opens = 0;
    std::size_t sets = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "open") {
            ++opens;
        }
        if (call.op == "set") {
            ++sets;
        }
    }
    return opens == 1 && sets == 1;
}

bool TestAcquireIdentityMismatchRejected() {
    // 观察时记录创建时间 999，实测为 111：PID 已重用，拒绝且不留句柄。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (booster.AcquireBoost("g", 100, 999, PriorityLevel::AboveNormal)
            .HasValue()) {
        return false;
    }
    if (booster.IsBoosted("g")) {
        return false;
    }
    for (const auto& call : fake->calls) {
        if (call.op == "set") {
            return false; // 身份不符绝不提升
        }
    }
    return fake->open.empty();
}

bool TestAcquireOpenFailureLeavesNoState() {
    auto fake = std::make_shared<FakeBackend>();
    PriorityBooster booster(fake);
    fake->failNextOpen = true;
    if (booster.AcquireBoost("g", 100, 0, PriorityLevel::AboveNormal)
            .HasValue()) {
        return false;
    }
    return !booster.IsBoosted("g") && fake->open.empty();
}

bool TestAcquireSetFailureClosesHandle() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    fake->failNextSet = true;
    if (booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)
            .HasValue()) {
        return false;
    }
    if (booster.IsBoosted("g") || booster.BoostCount("g") != 0) {
        return false;
    }
    // open 后 set 失败：紧跟 close（第 5 次调用），不留悬空句柄。
    if (fake->calls.size() != 5) {
        return false;
    }
    if (fake->calls[3].op != "set" || fake->calls[4].op != "close") {
        return false;
    }
    return fake->open.empty();
}

bool TestAcquireQueryClassFailureRejected() {
    // 无法读取原值（进程已退出/权限丢失）：不提升，句柄关闭。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    fake->failNextQueryClass = true;
    if (booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)
            .HasValue()) {
        return false;
    }
    return !booster.IsBoosted("g") && fake->open.empty();
}

bool TestAcquireRejectsNoneLevel() {
    auto fake = std::make_shared<FakeBackend>();
    PriorityBooster booster(fake);
    if (booster.AcquireBoost("g", 100, 0, PriorityLevel::None).HasValue()) {
        return false;
    }
    return fake->calls.empty() && !booster.IsBoosted("g");
}

bool TestAcquireRespectsMaxLevel() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster defaultBooster(fake); // maxLevel 默认 AboveNormal
    if (defaultBooster.AcquireBoost("g", 100, 111, PriorityLevel::High)
            .HasValue()) {
        return false; // High 需显式放开
    }
    PriorityBooster::Options options;
    options.maxLevel = PriorityLevel::High;
    PriorityBooster explicitBooster(fake, options);
    if (!explicitBooster.AcquireBoost("g", 100, 111, PriorityLevel::High)) {
        return false;
    }
    return explicitBooster.IsBoosted("g");
}

// ---------- Release（条件恢复） ----------

bool TestReleaseLastLeaseRestoresOriginal() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (booster.IsBoosted("g") || booster.BoostCount("g") != 0) {
        return false;
    }
    // 释放序列：qct(身份重验) -> qcls(当前值校验) -> set(原值) -> close。
    if (fake->calls.size() != 8) {
        return false;
    }
    if (fake->calls[4].op != "qct" || fake->calls[5].op != "qcls") {
        return false;
    }
    if (fake->calls[6].op != "set" ||
        fake->calls[6].value != FakeBackend::kNormalClass) {
        return false;
    }
    if (fake->calls[7].op != "close") {
        return false;
    }
    // 原优先级已恢复（NORMAL），句柄已关闭。
    if (fake->CurrentClass(100) != FakeBackend::kNormalClass) {
        return false;
    }
    return fake->open.empty();
}

bool TestMultiLeasePairedReleases() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    // 释放一次：仍持有，不触发恢复。
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (booster.BoostCount("g") != 1) {
        return false;
    }
    for (const auto& call : fake->calls) {
        if (call.op == "close") {
            return false;
        }
    }
    if (fake->CurrentClass(100) != FakeBackend::kAboveNormalClass) {
        return false;
    }
    // 再释放一次：恢复原值并关闭。
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (booster.IsBoosted("g")) {
        return false;
    }
    if (fake->CurrentClass(100) != FakeBackend::kNormalClass) {
        return false;
    }
    return fake->open.empty();
}

bool TestReleaseWhenNotHeldIsNoOp() {
    auto fake = std::make_shared<FakeBackend>();
    PriorityBooster booster(fake);
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (!booster.ReleaseBoost("missing", 200)) {
        return false;
    }
    return fake->calls.empty();
}

bool TestReleaseMismatchedPidRejected() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (booster.ReleaseBoost("g", 200).HasValue()) {
        return false; // 释放目标与租约不一致
    }
    // 状态不受影响：仍持有，句柄未关闭。
    return booster.IsBoosted("g") && booster.BoostCount("g") == 1 &&
           fake->open.size() == 1;
}

bool TestReleaseSkipsRestoreOnExternalChange() {
    // 外部工具把优先级改回 NORMAL：恢复必须跳过，不覆盖外部修改。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    fake->priorityByHandle[fake->handleByPid[100]] =
        FakeBackend::kNormalClass; // 模拟外部修改
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (booster.IsBoosted("g")) {
        return false;
    }
    // acquire 之后不得再出现 set（未覆盖外部值）。
    std::size_t sets = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "set") {
            ++sets;
        }
    }
    if (sets != 1) {
        return false;
    }
    if (fake->CurrentClass(100) != FakeBackend::kNormalClass) {
        return false;
    }
    return fake->open.empty();
}

bool TestReleaseRestoreFailureKeepsLease() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    fake->failNextSet = true;
    if (booster.ReleaseBoost("g", 100).HasValue()) {
        return false; // 恢复失败必须上报
    }
    // 恢复失败：计数恢复、仍持有、句柄保留（提升仍生效），可重试。
    if (booster.BoostCount("g") != 1 || !booster.IsBoosted("g")) {
        return false;
    }
    if (fake->open.empty()) {
        return false;
    }
    if (fake->CurrentClass(100) != FakeBackend::kAboveNormalClass) {
        return false;
    }
    // 重试释放成功。
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    return !booster.IsBoosted("g") && fake->open.empty() &&
           fake->CurrentClass(100) == FakeBackend::kNormalClass;
}

bool TestReleaseExitedProcessIsCancelled() {
    // 目标进程已退出：恢复跳过，视为正常取消，不报错。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    fake->failNextQueryClass = true; // 模拟进程退出后查询失败
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (booster.IsBoosted("g")) {
        return false;
    }
    std::size_t sets = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "set") {
            ++sets;
        }
    }
    return sets == 1 && fake->open.empty(); // 仅 acquire 时设置过
}

bool TestReleaseSkipsRestoreOnIdentityChange() {
    // 恢复前身份重验：创建时间变化视为进程被回收，放弃恢复。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    fake->creationByHandle[fake->handleByPid[100]] = 999; // 模拟身份变化
    if (!booster.ReleaseBoost("g", 100)) {
        return false;
    }
    if (booster.IsBoosted("g")) {
        return false;
    }
    std::size_t sets = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "set") {
            ++sets;
        }
    }
    return sets == 1 && fake->open.empty();
}

// ---------- 重启与多游戏 ----------

bool TestAcquireNewPidReplacesLease() {
    // 同 gameId 进程重启：旧租约条件释放（恢复原值）后全新获取。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    fake->creationByPid[200] = 222;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (!booster.AcquireBoost("g", 200, 222, PriorityLevel::AboveNormal)) {
        return false;
    }
    // 旧进程恢复原值并关闭；新进程提升中。
    if (fake->CurrentClass(100) != FakeBackend::kNormalClass) {
        return false;
    }
    if (fake->CurrentClass(200) != FakeBackend::kAboveNormalClass) {
        return false;
    }
    const auto states = booster.GetStates();
    if (states.size() != 1) {
        return false;
    }
    if (states[0].pid != 200 || states[0].count != 1) {
        return false;
    }
    return fake->open.size() == 1;
}

bool TestTwoGamesIndependent() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    fake->creationByPid[200] = 222;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("a", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (!booster.AcquireBoost("b", 200, 222, PriorityLevel::AboveNormal)) {
        return false;
    }
    if (fake->open.size() != 2) {
        return false;
    }
    if (!booster.ReleaseBoost("a", 100)) {
        return false;
    }
    // b 不受影响。
    if (!booster.IsBoosted("b")) {
        return false;
    }
    if (fake->open.size() != 1) {
        return false;
    }
    return fake->CurrentClass(200) == FakeBackend::kAboveNormalClass;
}

// ---------- RAII ----------

bool TestDtorReleasesAll() {
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    fake->creationByPid[200] = 222;
    {
        PriorityBooster booster(fake);
        if (!booster.AcquireBoost("a", 100, 111, PriorityLevel::AboveNormal)) {
            return false;
        }
        if (!booster.AcquireBoost("b", 200, 222, PriorityLevel::AboveNormal)) {
            return false;
        }
    } // 析构：自动 ReleaseAll
    if (!fake->open.empty()) {
        return false;
    }
    if (fake->CurrentClass(100) != FakeBackend::kNormalClass) {
        return false;
    }
    if (fake->CurrentClass(200) != FakeBackend::kNormalClass) {
        return false;
    }
    return true;
}

bool TestReleaseAllWithExternalChangeClears() {
    // ReleaseAll 也遵守"不覆盖外部修改"：外部已改值则不恢复原值，仅清理。
    auto fake = std::make_shared<FakeBackend>();
    fake->creationByPid[100] = 111;
    PriorityBooster booster(fake);
    if (!booster.AcquireBoost("g", 100, 111, PriorityLevel::AboveNormal)) {
        return false;
    }
    fake->priorityByHandle[fake->handleByPid[100]] =
        FakeBackend::kNormalClass; // 模拟外部修改
    booster.ReleaseAll();
    if (booster.IsBoosted("g")) {
        return false;
    }
    std::size_t sets = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "set") {
            ++sets;
        }
    }
    return sets == 1 && fake->open.empty();
}

bool TestCreateWin32Backend() {
    // 后端工厂恒成功（仅构造对象，不触碰 Win32）。
    auto backend = CreateWin32Backend();
    return backend != nullptr;
}

} // namespace

int wmain() {
    int failed = 0;
    const auto run = [&failed](const wchar_t* name, bool (*test)()) {
        const bool passed = test();
        std::wcout << (passed ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
        if (!passed) {
            ++failed;
        }
    };

    run(L"priority level maps to win32 class", &TestLevelToWin32Class);
    run(L"acquire opens verifies and boosts", &TestAcquireOpensVerifiesAndBoosts);
    run(L"double acquire same game keeps single open",
        &TestDoubleAcquireSameGameSingleOpen);
    run(L"acquire identity mismatch rejected", &TestAcquireIdentityMismatchRejected);
    run(L"acquire open failure leaves no state",
        &TestAcquireOpenFailureLeavesNoState);
    run(L"acquire set failure closes handle", &TestAcquireSetFailureClosesHandle);
    run(L"acquire query class failure rejected",
        &TestAcquireQueryClassFailureRejected);
    run(L"acquire rejects none level", &TestAcquireRejectsNoneLevel);
    run(L"acquire respects max level", &TestAcquireRespectsMaxLevel);
    run(L"release last lease restores original",
        &TestReleaseLastLeaseRestoresOriginal);
    run(L"multi-lease paired releases", &TestMultiLeasePairedReleases);
    run(L"release when not held is no-op", &TestReleaseWhenNotHeldIsNoOp);
    run(L"release mismatched pid rejected", &TestReleaseMismatchedPidRejected);
    run(L"release skips restore on external change",
        &TestReleaseSkipsRestoreOnExternalChange);
    run(L"release restore failure keeps lease",
        &TestReleaseRestoreFailureKeepsLease);
    run(L"release exited process is cancelled",
        &TestReleaseExitedProcessIsCancelled);
    run(L"release skips restore on identity change",
        &TestReleaseSkipsRestoreOnIdentityChange);
    run(L"acquire new pid replaces lease", &TestAcquireNewPidReplacesLease);
    run(L"two games are independent", &TestTwoGamesIndependent);
    run(L"dtor releases all leases", &TestDtorReleasesAll);
    run(L"release all with external change clears",
        &TestReleaseAllWithExternalChangeClears);
    run(L"create win32 backend", &TestCreateWin32Backend);
    return failed == 0 ? 0 : 1;
}
