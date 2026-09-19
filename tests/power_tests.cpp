#include "power/power_locker.hpp"

#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::Result;
using optimizer::power::CreateWin32Backend;
using optimizer::power::ParsePowerLockType;
using optimizer::power::PowerLocker;
using optimizer::power::PowerLockType;
using optimizer::power::PowerLockTypeToString;
using optimizer::power::PowerRequestBackend;

// 记录调用序列的 fake 后端：验证配对释放/引用计数/失败路径，
// 不触碰 Win32。句柄 id 从 1000 起，便于断言无悬空句柄。
class FakeBackend final : public PowerRequestBackend {
public:
    struct Call {
        std::string op; // create / set / clear / close
        std::uint64_t handle = 0;
        PowerLockType type = PowerLockType::ExecutionRequired;
        std::wstring reason;
    };

    bool failNextCreate = false;
    bool failNextSet = false;
    bool failNextClear = false;

    std::uint64_t nextHandle = 1000;
    std::vector<Call> calls;
    std::map<std::uint64_t, bool> open; // handle -> 仍打开

    Result<std::uint64_t> CreateRequest(
        std::wstring_view reason) override {
        calls.push_back(Call{"create", 0, PowerLockType::ExecutionRequired,
                             std::wstring(reason)});
        if (failNextCreate) {
            failNextCreate = false;
            return Result<std::uint64_t>::Failure(
                Error::Validation("FakeBackend::CreateRequest",
                                  L"injected create failure"));
        }
        const auto handle = nextHandle++;
        open[handle] = true;
        return Result<std::uint64_t>::Success(handle);
    }

    Result<void> SetRequest(std::uint64_t handle,
                            PowerLockType type) override {
        calls.push_back(Call{"set", handle, type, L""});
        if (failNextSet) {
            failNextSet = false;
            return Result<void>::Failure(
                Error::Validation("FakeBackend::SetRequest",
                                  L"injected set failure"));
        }
        return Result<void>::Success();
    }

    Result<void> ClearRequest(std::uint64_t handle,
                              PowerLockType type) override {
        calls.push_back(Call{"clear", handle, type, L""});
        if (failNextClear) {
            failNextClear = false;
            return Result<void>::Failure(
                Error::Validation("FakeBackend::ClearRequest",
                                  L"injected clear failure"));
        }
        return Result<void>::Success();
    }

    void CloseRequest(std::uint64_t handle) noexcept override {
        calls.push_back(
            Call{"close", handle, PowerLockType::ExecutionRequired, L""});
        open.erase(handle);
    }
};

// ---------- Acquire / Release 状态机 ----------

bool TestAcquireCreatesAndSets() {
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired,
                            L"test reason")) {
        return false;
    }
    if (!locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 1) {
        return false;
    }
    if (locker.LockReason(PowerLockType::ExecutionRequired) !=
        L"test reason") {
        return false;
    }
    // 调用序列：create + set 各一次，reason 传递给 create。
    if (fake->calls.size() != 2) {
        return false;
    }
    if (fake->calls[0].op != "create" ||
        fake->calls[0].reason != L"test reason") {
        return false;
    }
    if (fake->calls[1].op != "set" ||
        fake->calls[1].type != PowerLockType::ExecutionRequired) {
        return false;
    }
    // 句柄有效且打开。
    if (fake->calls[1].handle == 0) {
        return false;
    }
    return fake->open.count(fake->calls[1].handle) == 1;
}

bool TestDoubleAcquireSingleRequest() {
    // 同类型重复 Acquire：计数递增，但不重复创建/设置请求（仅一次 0->1）。
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"a")) {
        return false;
    }
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"b")) {
        return false;
    }
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 2) {
        return false;
    }
    std::size_t creates = 0;
    std::size_t sets = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "create") {
            ++creates;
        }
        if (call.op == "set") {
            ++sets;
        }
    }
    return creates == 1 && sets == 1 &&
           locker.IsLocked(PowerLockType::ExecutionRequired);
}

bool TestReasonFixedAtFirstCreate() {
    // reason 在 0->1 创建请求时写入，重复获取不改变已创建请求的 reason。
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"first")) {
        return false;
    }
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"second")) {
        return false;
    }
    return locker.LockReason(PowerLockType::ExecutionRequired) == L"first";
}

bool TestReleaseClearsAndCloses() {
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"r")) {
        return false;
    }
    const auto handle = fake->calls[1].handle; // set 的句柄
    if (!locker.ReleaseLock(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 0) {
        return false;
    }
    if (!locker.LockReason(PowerLockType::ExecutionRequired).empty()) {
        return false;
    }
    // 释放序列：clear + close，句柄一致。
    if (fake->calls.size() != 4) {
        return false;
    }
    if (fake->calls[2].op != "clear" ||
        fake->calls[2].handle != handle) {
        return false;
    }
    if (fake->calls[3].op != "close" ||
        fake->calls[3].handle != handle) {
        return false;
    }
    return fake->open.empty();
}

bool TestReleaseWhenNotHeldIsNoOp() {
    // 幂等：对未持有类型的 ReleaseLock 为空操作成功，不触发后端调用。
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.ReleaseLock(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (!locker.ReleaseLock(PowerLockType::DisplayRequired)) {
        return false;
    }
    return fake->calls.empty();
}

bool TestRefCountPairedReleases() {
    // 两次获取 + 一次释放：仍持有（计数 1），计数归零前不触发 clear/close。
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")) {
        return false;
    }
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")) {
        return false;
    }
    if (!locker.ReleaseLock(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 1) {
        return false;
    }
    if (!locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    for (const auto& call : fake->calls) {
        if (call.op == "clear" || call.op == "close") {
            return false;
        }
    }
    // 再释放一次：计数归零，clear + close。
    if (!locker.ReleaseLock(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (fake->calls.size() != 4) {
        return false;
    }
    return fake->open.empty();
}

bool TestReleaseAllClearsAll() {
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"e")) {
        return false;
    }
    if (!locker.AcquireLock(PowerLockType::DisplayRequired, L"d")) {
        return false;
    }
    locker.ReleaseAll();
    if (locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.IsLocked(PowerLockType::DisplayRequired)) {
        return false;
    }
    // 每种类型都有 clear + close。
    std::size_t clears = 0;
    std::size_t closes = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "clear") {
            ++clears;
        }
        if (call.op == "close") {
            ++closes;
        }
    }
    if (clears != 2 || closes != 2) {
        return false;
    }
    return fake->open.empty();
}

bool TestTypesIndependent() {
    // 两种类型互不影响：各自独立计数、独立句柄。
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"e")) {
        return false;
    }
    if (!locker.AcquireLock(PowerLockType::DisplayRequired, L"d")) {
        return false;
    }
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 1 ||
        locker.LockCount(PowerLockType::DisplayRequired) != 1) {
        return false;
    }
    if (fake->open.size() != 2) {
        return false;
    }
    if (!locker.ReleaseLock(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    // display 不受影响，句柄仍打开。
    if (!locker.IsLocked(PowerLockType::DisplayRequired)) {
        return false;
    }
    return fake->open.size() == 1;
}

// ---------- 失败路径（失败不伪装成功） ----------

bool TestAcquireCreateFailureKeepsState() {
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    fake->failNextCreate = true;
    if (locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")
            .HasValue()) {
        return false; // 应失败
    }
    if (locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 0) {
        return false;
    }
    if (!fake->open.empty()) {
        return false; // 无悬空句柄
    }
    // 失败后可重试成功。
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")) {
        return false;
    }
    return locker.IsLocked(PowerLockType::ExecutionRequired);
}

bool TestAcquireSetFailureClosesHandle() {
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    fake->failNextSet = true;
    if (locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")
            .HasValue()) {
        return false;
    }
    if (locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    // create 后 set 失败，紧跟 close（不留悬空句柄）；计数保持 0。
    if (fake->calls.size() != 3) {
        return false;
    }
    if (fake->calls[0].op != "create" || fake->calls[1].op != "set" ||
        fake->calls[2].op != "close") {
        return false;
    }
    return fake->open.empty();
}

bool TestReleaseClearFailureKeepsHeld() {
    auto fake = std::make_shared<FakeBackend>();
    PowerLocker locker(fake);
    if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")) {
        return false;
    }
    fake->failNextClear = true;
    if (locker.ReleaseLock(PowerLockType::ExecutionRequired).HasValue()) {
        return false; // 应失败
    }
    // 清除失败：计数恢复、仍持有、句柄保留，可重试释放。
    if (locker.LockCount(PowerLockType::ExecutionRequired) != 1) {
        return false;
    }
    if (!locker.IsLocked(PowerLockType::ExecutionRequired)) {
        return false;
    }
    if (fake->open.empty()) {
        return false; // 句柄未关闭
    }
    if (!locker.ReleaseLock(PowerLockType::ExecutionRequired)) {
        return false;
    }
    return !locker.IsLocked(PowerLockType::ExecutionRequired) &&
           fake->open.empty();
}

// ---------- RAII ----------

bool TestDtorReleasesAutomatically() {
    auto fake = std::make_shared<FakeBackend>();
    {
        PowerLocker locker(fake);
        if (!locker.AcquireLock(PowerLockType::ExecutionRequired, L"x")) {
            return false;
        }
        if (!locker.AcquireLock(PowerLockType::DisplayRequired, L"y")) {
            return false;
        }
    } // 析构：自动 ReleaseAll
    if (!fake->open.empty()) {
        return false; // 全部句柄已关闭
    }
    std::size_t clears = 0;
    std::size_t closes = 0;
    for (const auto& call : fake->calls) {
        if (call.op == "clear") {
            ++clears;
        }
        if (call.op == "close") {
            ++closes;
        }
    }
    return clears == 2 && closes == 2;
}

// ---------- 纯函数 ----------

bool TestParseLockType() {
    // 大小写不敏感（ASCII 折叠）。
    if (ParsePowerLockType("execution").Value() !=
        PowerLockType::ExecutionRequired) {
        return false;
    }
    if (ParsePowerLockType("DISPLAY").Value() !=
        PowerLockType::DisplayRequired) {
        return false;
    }
    if (ParsePowerLockType("Execution").Value() !=
        PowerLockType::ExecutionRequired) {
        return false;
    }
    // 非法名拒绝（both 属 CLI 层概念，不在此枚举）。
    if (ParsePowerLockType("both").HasValue()) {
        return false;
    }
    if (ParsePowerLockType("").HasValue()) {
        return false;
    }
    if (ParsePowerLockType("realtime").HasValue()) {
        return false;
    }
    return true;
}

bool TestLockTypeToString() {
    if (std::wstring_view(PowerLockTypeToString(
            PowerLockType::ExecutionRequired)) != L"execution") {
        return false;
    }
    if (std::wstring_view(PowerLockTypeToString(
            PowerLockType::DisplayRequired)) != L"display") {
        return false;
    }
    return true;
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

    run(L"acquire creates and sets request", &TestAcquireCreatesAndSets);
    run(L"double acquire keeps single request", &TestDoubleAcquireSingleRequest);
    run(L"reason fixed at first create", &TestReasonFixedAtFirstCreate);
    run(L"release clears and closes", &TestReleaseClearsAndCloses);
    run(L"release when not held is no-op", &TestReleaseWhenNotHeldIsNoOp);
    run(L"ref-count paired releases", &TestRefCountPairedReleases);
    run(L"release all clears all types", &TestReleaseAllClearsAll);
    run(L"types are independent", &TestTypesIndependent);
    run(L"acquire create failure keeps state", &TestAcquireCreateFailureKeepsState);
    run(L"acquire set failure closes handle", &TestAcquireSetFailureClosesHandle);
    run(L"release clear failure keeps held", &TestReleaseClearFailureKeepsHeld);
    run(L"dtor releases automatically", &TestDtorReleasesAutomatically);
    run(L"parse power lock type", &TestParseLockType);
    run(L"power lock type to string", &TestLockTypeToString);
    run(L"create win32 backend", &TestCreateWin32Backend);
    return failed == 0 ? 0 : 1;
}
