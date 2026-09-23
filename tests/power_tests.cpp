#include "power/power_locker.hpp"
#include "power/power_scheme.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
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

bool TestSchemeGuidPureFunctions() {
    using optimizer::power::NormalizeSchemeGuid;
    using optimizer::power::ResolveSchemeArgument;
    // 归一化：去大括号与空白、转大写；长度/分隔/非十六进制字符一律拒绝（不宽松转换）。
    const auto braces = NormalizeSchemeGuid("{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}");
    const auto spaced =
        NormalizeSchemeGuid(" 8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C ");
    const bool normalized =
        braces.has_value() &&
        *braces == "8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C" && braces == spaced;
    const bool rejected = !NormalizeSchemeGuid("").has_value() &&
                          !NormalizeSchemeGuid("8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635").has_value() &&
                          !NormalizeSchemeGuid("8C5E7FDA_E8BF_4A96_9A85_A6E23A8C635C").has_value() &&
                          !NormalizeSchemeGuid("8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635Z").has_value();
    // 别名解析：大小写不敏感 + 下划线等价横线；也接受直接给出的 GUID；未知一律 nullopt。
    const auto alias = ResolveSchemeArgument("High_Performance");
    const auto direct = ResolveSchemeArgument("{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}");
    const bool resolved =
        alias.has_value() && *alias == optimizer::power::kSchemeHighPerformance &&
        direct.has_value() && *direct == *alias &&
        ResolveSchemeArgument("power-saver").has_value() &&
        ResolveSchemeArgument("balanced").has_value();
    const bool unknownRejected = !ResolveSchemeArgument("turbo").has_value() &&
                                 !ResolveSchemeArgument("high performance").has_value();
    return normalized && rejected && resolved && unknownRejected;
}

// fake 后端：可注入读/写失败，并记录调用顺序（不触碰真实电源计划）。
class FakeSchemeBackend final : public optimizer::power::PowerSchemeBackend {
public:
    explicit FakeSchemeBackend(std::string active)
        : active_(std::move(active)) {}

    Result<std::string> GetActive() override {
        if (failGet) {
            return Result<std::string>::Failure(
                Error::FromWin32(5, "PowerGetActiveScheme"));
        }
        return Result<std::string>::Success(active_);
    }

    Result<void> SetActive(std::string_view guid) override {
        writes.push_back(std::string(guid));
        if (observePath != nullptr) {
            std::ifstream in(*observePath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            sawPendingRecordAtSet = content.find("state=pending") != std::string::npos;
        }
        if (failSet) {
            return Result<void>::Failure(Error::FromWin32(5, "PowerSetActiveScheme"));
        }
        if (!ignoreSet) {
            active_ = std::string(guid);
        }
        return Result<void>::Success();
    }

    bool failGet = false;
    bool failSet = false;
    bool ignoreSet = false; // 模拟“调用成功但状态未变”
    std::vector<std::string> writes;
    // 若设置：在 SetActive 调用瞬间读该文件，判断记录是否已是 pending（验证“先落盘后切换”）。
    const std::filesystem::path* observePath = nullptr;
    bool sawPendingRecordAtSet = false;

private:
    std::string active_;
};

bool TestApplyAndRestoreScheme() {
    using optimizer::power::ApplyScheme;
    using optimizer::power::kSchemeHighPerformance;
    using optimizer::power::kSchemePowerSaver;
    using optimizer::power::RestoreScheme;
    // 正常路径：保存原 GUID -> 切换 -> 读回校验 -> 恢复 -> 读回校验。
    FakeSchemeBackend backend{std::string(kSchemePowerSaver)};
    const auto applied = ApplyScheme(backend, kSchemeHighPerformance);
    const bool appliedOk =
        applied && applied.Value().previousGuid == std::string(kSchemePowerSaver) &&
        applied.Value().targetGuid == std::string(kSchemeHighPerformance) &&
        applied.Value().activeGuid == std::string(kSchemeHighPerformance) &&
        applied.Value().changed;
    const auto restored = RestoreScheme(backend, applied.Value().previousGuid);
    const bool restoredOk =
        restored && restored.Value().savedGuid == std::string(kSchemePowerSaver) &&
        restored.Value().activeGuid == std::string(kSchemePowerSaver) &&
        restored.Value().restored;
    // 读不到原 GUID -> **拒绝切换**（一步也不写）。
    FakeSchemeBackend blind{std::string(kSchemePowerSaver)};
    blind.failGet = true;
    const auto noRollbackInfo = ApplyScheme(blind, kSchemeHighPerformance);
    const bool refusedWithoutRollback = !noRollbackInfo && blind.writes.empty();
    // 切换调用失败 -> Failure 且不读回。
    FakeSchemeBackend writeFails{std::string(kSchemePowerSaver)};
    writeFails.failSet = true;
    const auto writeFailure = ApplyScheme(writeFails, kSchemeHighPerformance);
    const bool writeRefused = !writeFailure &&
                              writeFailure.ErrorValue().domain ==
                                  optimizer::common::ErrorDomain::Win32;
    // “调用成功但状态未变” -> changed=false（不冒充已切换）。
    FakeSchemeBackend silent{std::string(kSchemePowerSaver)};
    silent.ignoreSet = true;
    const auto notChanged = ApplyScheme(silent, kSchemeHighPerformance);
    const bool changedReportedHonest = notChanged && !notChanged.Value().changed;
    // 非法 GUID -> Validation 且零写入。
    FakeSchemeBackend invalid{std::string(kSchemePowerSaver)};
    const auto invalidTarget = ApplyScheme(invalid, "not-a-guid");
    const bool invalidRefused =
        !invalidTarget && invalid.writes.empty() &&
        invalidTarget.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
    // 恢复同样做读回校验。
    FakeSchemeBackend restoreSilent{std::string(kSchemeHighPerformance)};
    restoreSilent.ignoreSet = true;
    const auto notRestored = RestoreScheme(restoreSilent, kSchemePowerSaver);
    const bool restoreHonest = notRestored && !notRestored.Value().restored;
    return appliedOk && restoredOk && refusedWithoutRollback && writeRefused &&
           changedReportedHonest && invalidRefused && restoreHonest;
}

// 每用户记录文件路径（临时目录，测试后用后即删）。
std::filesystem::path TempSchemeSavePath(const wchar_t* tag) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec);
    return dir / (std::wstring(L"cpo_scheme_") + tag + L"_" +
                  std::to_wstring(::GetCurrentProcessId()) + L".txt");
}

bool TestSchemeSaveRecordParsingAndIo() {
    using optimizer::power::ParseSavedSchemeRecord;
    using optimizer::power::ReadSavedSchemeRecord;
    using optimizer::power::SaveSchemeRecord;
    using optimizer::power::SavedSchemeRecord;
    using optimizer::power::SchemeSaveState;
    const std::string applied = std::string(optimizer::power::kSchemeSaveEnvelope) +
                                "\nstate=applied\nsaved=" +
                                std::string(optimizer::power::kSchemePowerSaver) +
                                "\ntarget=" +
                                std::string(optimizer::power::kSchemeHighPerformance) +
                                "\n";
    const auto parsed = ParseSavedSchemeRecord(applied);
    const bool parseOk =
        parsed.has_value() && parsed->state == SchemeSaveState::Applied &&
        parsed->savedGuid == optimizer::power::kSchemePowerSaver &&
        parsed->targetGuid.has_value() &&
        *parsed->targetGuid == optimizer::power::kSchemeHighPerformance;
    // 缺状态 / 未知状态 / 缺 saved / 非法 GUID / 信封不符 -> 一律不接受。
    const bool rejected =
        !ParseSavedSchemeRecord(std::string(optimizer::power::kSchemeSaveEnvelope) +
                                "\nsaved=" +
                                std::string(optimizer::power::kSchemePowerSaver) + "\n")
             .has_value() &&
        !ParseSavedSchemeRecord(std::string(optimizer::power::kSchemeSaveEnvelope) +
                                "\nstate=maybe\nsaved=" +
                                std::string(optimizer::power::kSchemePowerSaver) + "\n")
             .has_value() &&
        !ParseSavedSchemeRecord(std::string(optimizer::power::kSchemeSaveEnvelope) +
                                "\nstate=applied\n")
             .has_value() &&
        !ParseSavedSchemeRecord(std::string(optimizer::power::kSchemeSaveEnvelope) +
                                "\nstate=applied\nsaved=not-a-guid\n")
             .has_value() &&
        !ParseSavedSchemeRecord("OtherEnvelope/1\nstate=applied\nsaved=" +
                                std::string(optimizer::power::kSchemePowerSaver) + "\n")
             .has_value();
    // 文件往返（含 pending + target）+ 原子替换不留 .tmp；不可信内容读回为“无记录”。
    const auto path = TempSchemeSavePath(L"io");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    SavedSchemeRecord record;
    record.savedGuid = std::string(optimizer::power::kSchemePowerSaver);
    record.targetGuid = std::string(optimizer::power::kSchemeHighPerformance);
    record.state = SchemeSaveState::Pending;
    const auto written = SaveSchemeRecord(path, record);
    const auto read = ReadSavedSchemeRecord(path);
    const bool roundTrip =
        written && read && read.Value().has_value() &&
        read.Value()->state == SchemeSaveState::Pending &&
        read.Value()->savedGuid == record.savedGuid;
    std::filesystem::path tempPath = path;
    tempPath += L".tmp";
    const bool noTempLeft = !std::filesystem::exists(tempPath, ec);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "garbage\n";
    }
    const auto garbage = ReadSavedSchemeRecord(path);
    const bool garbageRejected = garbage && !garbage.Value().has_value();
    const auto emptyPath = SaveSchemeRecord({}, record);
    const auto badGuid = SaveSchemeRecord(path, SavedSchemeRecord{"nope", std::nullopt,
                                                                 SchemeSaveState::Pending});
    const bool refusedBadInput =
        !emptyPath && !badGuid &&
        emptyPath.ErrorValue().domain == optimizer::common::ErrorDomain::Validation &&
        badGuid.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
    std::filesystem::remove(path, ec);
    return parseOk && rejected && roundTrip && noTempLeft && garbageRejected &&
           refusedBadInput;
}

bool TestSchemeRecoveryDecision() {
    using optimizer::power::DecideSchemeRecovery;
    using optimizer::power::SavedSchemeRecord;
    using optimizer::power::SchemeRecoveryAction;
    using optimizer::power::SchemeSaveState;
    const std::string saved(optimizer::power::kSchemePowerSaver);
    const std::string other(optimizer::power::kSchemeHighPerformance);
    SavedSchemeRecord pending;
    pending.savedGuid = saved;
    pending.targetGuid = other;
    pending.state = SchemeSaveState::Pending;
    SavedSchemeRecord applied = pending;
    applied.state = SchemeSaveState::Applied;
    SavedSchemeRecord restored = pending;
    restored.state = SchemeSaveState::Restored;
    const bool noRecord = DecideSchemeRecovery(std::nullopt, saved) ==
                          SchemeRecoveryAction::None;
    const bool appliedNone =
        DecideSchemeRecovery(applied, other) == SchemeRecoveryAction::None;
    const bool restoredNone =
        DecideSchemeRecovery(restored, other) == SchemeRecoveryAction::None;
    const bool pendingDiffers = DecideSchemeRecovery(pending, other) ==
                                SchemeRecoveryAction::RestoreToSaved;
    const bool pendingSame = DecideSchemeRecovery(pending, saved) ==
                             SchemeRecoveryAction::MarkNeverApplied;
    // 实际状态未知（空串/非法）-> 不凭猜测动作。
    const bool unknownActive =
        DecideSchemeRecovery(pending, "") == SchemeRecoveryAction::None;
    return noRecord && appliedNone && restoredNone && pendingDiffers && pendingSame &&
           unknownActive;
}

bool TestSchemeFlowWithRecords() {
    using optimizer::power::ApplySchemeWithRecord;
    using optimizer::power::kSchemeHighPerformance;
    using optimizer::power::kSchemePowerSaver;
    using optimizer::power::ReadSavedSchemeRecord;
    using optimizer::power::RecoverPendingScheme;
    using optimizer::power::RestoreSchemeWithRecord;
    using optimizer::power::SchemeRecoveryAction;
    using optimizer::power::SchemeSaveState;
    const auto path = TempSchemeSavePath(L"flow");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 正常路径：**先落盘 pending（在 SetActive 瞬间核对）** -> 切换 -> 读回 -> 记录变 applied。
    FakeSchemeBackend backend{std::string(kSchemePowerSaver)};
    backend.observePath = &path;
    const auto applied = ApplySchemeWithRecord(backend, path, kSchemeHighPerformance);
    const auto afterApply = ReadSavedSchemeRecord(path);
    const bool applyOk = applied && applied.Value().changed &&
                         applied.Value().recordMarkedApplied &&
                         backend.sawPendingRecordAtSet && afterApply &&
                         afterApply.Value().has_value() &&
                         afterApply.Value()->state == SchemeSaveState::Applied &&
                         afterApply.Value()->savedGuid == std::string(kSchemePowerSaver);
    // 回滚（带记录）：切换回保存值并把记录标 restored。
    const auto rolledBack = RestoreSchemeWithRecord(backend, path);
    const auto afterRestore = ReadSavedSchemeRecord(path);
    const bool restoreOk =
        rolledBack && rolledBack.Value().restored &&
        rolledBack.Value().recordMarkedRestored && afterRestore &&
        afterRestore.Value().has_value() &&
        afterRestore.Value()->state == SchemeSaveState::Restored;
    // 读回不一致 -> **记录保持 pending**（下次启动保守回滚）。
    std::filesystem::remove(path, ec);
    FakeSchemeBackend silent{std::string(kSchemePowerSaver)};
    silent.ignoreSet = true;
    const auto notChanged = ApplySchemeWithRecord(silent, path, kSchemeHighPerformance);
    const auto pendingAfterMismatch = ReadSavedSchemeRecord(path);
    const bool mismatchKeepsPending =
        notChanged && !notChanged.Value().changed && pendingAfterMismatch &&
        pendingAfterMismatch.Value().has_value() &&
        pendingAfterMismatch.Value()->state == SchemeSaveState::Pending;
    // 读不到原 GUID -> 拒绝且**不写记录**。
    std::filesystem::remove(path, ec);
    FakeSchemeBackend blind{std::string(kSchemePowerSaver)};
    blind.failGet = true;
    const auto noInfo = ApplySchemeWithRecord(blind, path, kSchemeHighPerformance);
    const bool noInfoRefused = !noInfo &&
                               !std::filesystem::exists(path, ec) &&
                               blind.writes.empty();
    // 恢复依据写不进去（路径是目录）-> 拒绝切换（零后端写入）。
    FakeSchemeBackend dirRefused{std::string(kSchemePowerSaver)};
    const auto dirPath = std::filesystem::temp_directory_path(ec);
    const auto cannotPersist = ApplySchemeWithRecord(dirRefused, dirPath,
                                                    kSchemeHighPerformance);
    const bool persistRefused = !cannotPersist && dirRefused.writes.empty();
    // 崩溃恢复：pending + 实际 ≠ 保存值 -> 回滚并标 restored。
    std::filesystem::remove(path, ec);
    FakeSchemeBackend crashed{std::string(kSchemeHighPerformance)};
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << optimizer::power::kSchemeSaveEnvelope << "\nstate=pending\nsaved="
            << kSchemePowerSaver << "\ntarget=" << kSchemeHighPerformance << "\n";
    }
    const auto recovered = RecoverPendingScheme(crashed, path);
    const auto afterRecover = ReadSavedSchemeRecord(path);
    const bool recoverOk =
        recovered && recovered.Value().action == SchemeRecoveryAction::RestoreToSaved &&
        recovered.Value().restored && crashed.writes.size() == 1 &&
        crashed.writes[0] == std::string(kSchemePowerSaver) && afterRecover &&
        afterRecover.Value().has_value() &&
        afterRecover.Value()->state == SchemeSaveState::Restored;
    // 崩溃恢复：pending + 实际 == 保存值 -> 切换从未生效（不动系统、只改状态）。
    std::filesystem::remove(path, ec);
    FakeSchemeBackend neverApplied{std::string(kSchemePowerSaver)};
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << optimizer::power::kSchemeSaveEnvelope << "\nstate=pending\nsaved="
            << kSchemePowerSaver << "\n";
    }
    const auto marked = RecoverPendingScheme(neverApplied, path);
    const bool neverAppliedOk =
        marked &&
        marked.Value().action == SchemeRecoveryAction::MarkNeverApplied &&
        !marked.Value().restored && neverApplied.writes.empty();
    // applied 记录 -> 不自动回滚（有意保留）。
    std::filesystem::remove(path, ec);
    FakeSchemeBackend kept{std::string(kSchemeHighPerformance)};
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << optimizer::power::kSchemeSaveEnvelope << "\nstate=applied\nsaved="
            << kSchemePowerSaver << "\n";
    }
    const auto keptResult = RecoverPendingScheme(kept, path);
    const bool keptOk = keptResult &&
                        keptResult.Value().action == SchemeRecoveryAction::None &&
                        kept.writes.empty();
    std::filesystem::remove(path, ec);
    return applyOk && restoreOk && mismatchKeepsPending && noInfoRefused &&
           persistRefused && recoverOk && neverAppliedOk && keptOk;
}

bool TestWin32SchemeBackendReadOnly() {
    // 真实后端**只读**调用（不切换）：读回当前活动电源计划 GUID 必须是合法形式。
    const auto active = optimizer::power::Win32PowerSchemeBackend().GetActive();
    if (!active) {
        return false;
    }
    return optimizer::power::NormalizeSchemeGuid(active.Value()).has_value();
}

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
    run(L"scheme guid pure functions", &TestSchemeGuidPureFunctions);
    run(L"apply and restore scheme", &TestApplyAndRestoreScheme);
    run(L"scheme save record parsing and io", &TestSchemeSaveRecordParsingAndIo);
    run(L"scheme recovery decision", &TestSchemeRecoveryDecision);
    run(L"scheme flow with records", &TestSchemeFlowWithRecords);
    run(L"win32 scheme backend read only", &TestWin32SchemeBackendReadOnly);
    return failed == 0 ? 0 : 1;
}
