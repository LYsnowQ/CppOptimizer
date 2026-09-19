#include "audit/audit_log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>

namespace {

using optimizer::audit::AuditLog;
using optimizer::audit::AuditRecord;
using optimizer::audit::FormatAuditRecord;
using optimizer::audit::RiskLevel;
using optimizer::common::Error;
using optimizer::common::Result;

AuditRecord MakeRecord(std::string op, bool ok = true) {
    AuditRecord r;
    r.operationId = std::move(op);
    r.risk = RiskLevel::R1;
    r.caller = "test";
    r.target = "g pid 100";
    r.detail = "unit test";
    r.ok = ok;
    return r;
}

bool TestAuditAppendStoresOrdered() {
    AuditLog log(AuditLog::Options{});
    if (!log.IsAvailable() || log.Size() != 0) {
        return false;
    }
    const auto first = log.Append(MakeRecord("priority.boost"));
    const auto second = log.Append(MakeRecord("power.hold", false));
    if (!first || !second) {
        return false;
    }
    const auto records = log.Records();
    if (records.size() != 2 || log.Size() != 2) {
        return false;
    }
    return records[0].operationId == "priority.boost" && records[0].ok &&
           records[1].operationId == "power.hold" && !records[1].ok &&
           records[0].at != std::chrono::steady_clock::time_point{};
}

bool TestAuditRingDropsOldest() {
    AuditLog log(AuditLog::Options{2, true});
    (void)log.Append(MakeRecord("a"));
    (void)log.Append(MakeRecord("b"));
    (void)log.Append(MakeRecord("c"));
    if (log.Size() != 2) {
        return false;
    }
    const auto records = log.Records();
    return records.size() == 2 && records[0].operationId == "b" &&
           records[1].operationId == "c";
}

bool TestAuditDisabledIsUnavailable() {
    AuditLog log(AuditLog::Options{128, false});
    if (log.IsAvailable()) {
        return false;
    }
    const auto result = log.Append(MakeRecord("priority.boost"));
    return !result && log.Size() == 0; // 审计不可用：Append 如实失败
}

bool TestAuditFormatRoundTrip() {
    const auto text = FormatAuditRecord(MakeRecord("priority.boost", true));
    const auto failed = FormatAuditRecord(MakeRecord("power.release", false));
    return text.find(L"R1") != std::wstring::npos &&
           text.find(L"priority.boost") != std::wstring::npos &&
           text.find(L"ok") != std::wstring::npos &&
           failed.find(L"fail") != std::wstring::npos;
}

bool TestAuditRecordsCopyIsolated() {
    AuditLog log(AuditLog::Options{});
    (void)log.Append(MakeRecord("a"));
    auto copy = log.Records();
    copy.clear();
    return log.Size() == 1; // Records 返回拷贝，不影响内部
}

// ---------- AUD-002：审计记录文件持久化 ----------

// 测试用临时路径（进程内唯一，避免并行 ctest 互踩）。
std::filesystem::path TempAuditPath(const wchar_t* name) {
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return {};
    }
    return root / (std::wstring(L"cpo_audit_") + name + L"_" +
                   std::to_wstring(::GetCurrentProcessId()) + L".log");
}

bool TestAppendAuditLineRejectsEmptyPath() {
    using optimizer::audit::AppendAuditLine;
    const auto result = AppendAuditLine({}, MakeRecord("priority.boost", true));
    return !result &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestAppendAuditLineAppendsReadableLines() {
    using optimizer::audit::AppendAuditLine;
    const auto path = TempAuditPath(L"append");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto ok = MakeRecord("priority.boost", true);
    ok.caller = "policy";
    ok.target = "g pid 100";
    ok.detail = "hold";
    auto failed = MakeRecord("power.release", false);
    if (!AppendAuditLine(path, ok) || !AppendAuditLine(path, failed)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::string line1;
    std::string line2;
    std::getline(in, line1);
    std::getline(in, line2);
    in.close();
    const bool firstOk =
        line1.find("[audit] R1 priority.boost ok") != std::string::npos &&
        line1.find("caller=policy") != std::string::npos &&
        line1.find("target=g pid 100") != std::string::npos;
    const bool secondOk =
        line2.find("[audit] R1 power.release fail") != std::string::npos;
    // 行首本地时间戳 "YYYY-MM-DD HH:MM:SS "（ASCII，可 grep/排序）。
    const bool stampOk = line1.size() > 20 && line1[4] == '-' &&
                         line1[7] == '-' && line1[10] == ' ' &&
                         line1[13] == ':' && line1[16] == ':';
    std::filesystem::remove(path, ec);
    return firstOk && secondOk && stampOk;
}

bool TestAppendAuditLineCreatesParentDirectories() {
    using optimizer::audit::AppendAuditLine;
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return false;
    }
    const auto dir =
        root / (L"cpo_audit_nested_" +
                std::to_wstring(::GetCurrentProcessId()));
    const auto path = dir / L"sub" / L"audit.log";
    std::filesystem::remove_all(dir, ec);
    const bool ok = static_cast<bool>(
        AppendAuditLine(path, MakeRecord("priority.unboost")));
    const bool exists = std::filesystem::exists(path, ec);
    std::filesystem::remove_all(dir, ec);
    return ok && exists;
}

bool TestAppendAuditLineKeepsOneLinePerRecord() {
    using optimizer::audit::AppendAuditLine;
    const auto path = TempAuditPath(L"oneline");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto record = MakeRecord("priority.boost", true);
    record.target = "line1\r\nline2\ttab"; // 控制字符不得拆行
    if (!AppendAuditLine(path, record)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::string line1;
    std::string line2;
    std::getline(in, line1);
    const bool hasSecond = static_cast<bool>(std::getline(in, line2));
    in.close();
    std::filesystem::remove(path, ec);
    return !hasSecond && line1.find("line1  line2 tab") != std::string::npos;
}

bool TestAuditLogPersistsToFile() {
    const auto path = TempAuditPath(L"persist");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    AuditLog::Options options;
    options.filePath = path;
    AuditLog log(options);
    const bool okAppend = static_cast<bool>(log.Append(MakeRecord("a")));
    const bool okSecond = static_cast<bool>(log.Append(MakeRecord("b")));
    std::ifstream in(path, std::ios::binary);
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lines;
    }
    in.close();
    std::filesystem::remove(path, ec);
    return log.IsPersistent() && okAppend && okSecond && log.Size() == 2 &&
           lines == 2;
}

bool TestAuditLogPersistenceFailureNotFaked() {
    // 目录当文件：打开必失败 -> Append 返回 Failure，且不得留下“已记录”的假象。
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return false;
    }
    const auto dir = root / (L"cpo_audit_dir_" +
                             std::to_wstring(::GetCurrentProcessId()));
    std::filesystem::create_directories(dir, ec);
    AuditLog::Options options;
    options.filePath = dir;
    AuditLog log(options);
    const auto result = log.Append(MakeRecord("a"));
    const bool failed = !result && log.Size() == 0 && log.Records().empty();
    std::filesystem::remove_all(dir, ec);
    return failed;
}

bool TestAuditLogWithoutFilePathStaysInMemory() {
    // 零回归：未配置路径时不触碰磁盘（AUD-001 行为）。
    AuditLog log(AuditLog::Options{});
    const bool appended = static_cast<bool>(log.Append(MakeRecord("a")));
    return !log.IsPersistent() && appended && log.Size() == 1;
}

// ---------- AUD-003：审计记录只读回看 ----------

bool TestReadAuditTailRejectsEmptyPath() {
    using optimizer::audit::ReadAuditTail;
    const auto result = ReadAuditTail({}, 10);
    return !result &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestReadAuditTailMissingFileIsNotAnError() {
    // “尚无审计记录”不是错误：不得伪装成失败，也不得伪装成有内容。
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"missing");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto result = ReadAuditTail(path, 10);
    return result && result.Value().totalLines == 0 &&
           result.Value().lines.empty();
}

bool TestReadAuditTailReturnsLastLines() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"tail");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (int i = 1; i <= 3; ++i) {
        if (!AppendAuditLine(path, MakeRecord("op" + std::to_string(i)))) {
            return false;
        }
    }
    const auto result = ReadAuditTail(path, 2);
    if (!result) {
        return false;
    }
    const auto& tail = result.Value();
    const bool oldestFirst =
        tail.lines.size() == 2 &&
        tail.lines[0].find("op2") != std::string::npos &&
        tail.lines[1].find("op3") != std::string::npos;
    std::filesystem::remove(path, ec);
    return tail.totalLines == 3 && oldestFirst;
}

bool TestReadAuditTailZeroLinesCountsOnly() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"countonly");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (!AppendAuditLine(path, MakeRecord("a"))) {
        return false;
    }
    const auto result = ReadAuditTail(path, 0);
    const bool ok = result && result.Value().totalLines == 1 &&
                    result.Value().lines.empty();
    std::filesystem::remove(path, ec);
    return ok;
}

bool TestReadAuditTailTruncatesLongLine() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"longline");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto record = MakeRecord("a");
    record.detail = std::string(5000, 'x'); // 超 4096：截断而不无界分配
    if (!AppendAuditLine(path, record)) {
        return false;
    }
    const auto result = ReadAuditTail(path, 1);
    const bool ok = result && result.Value().totalLines == 1 &&
                    result.Value().lines.size() == 1 &&
                    result.Value().lines[0].size() == 4096 + 3 &&
                    result.Value().lines[0].compare(4096, 3, "...") == 0;
    std::filesystem::remove(path, ec);
    return ok;
}

bool TestReadAuditTailDirectoryIsFailure() {
    // 目录当文件：读取失败必须如实返回，不得伪装成“无记录”。
    using optimizer::audit::ReadAuditTail;
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return false;
    }
    const auto dir = root / (L"cpo_audit_read_dir_" +
                             std::to_wstring(::GetCurrentProcessId()));
    std::filesystem::create_directories(dir, ec);
    const auto result = ReadAuditTail(dir, 5);
    const bool failed = !result;
    std::filesystem::remove_all(dir, ec);
    return failed;
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

    run(L"audit append stores ordered", &TestAuditAppendStoresOrdered);
    run(L"audit ring drops oldest", &TestAuditRingDropsOldest);
    run(L"audit disabled unavailable", &TestAuditDisabledIsUnavailable);
    run(L"audit format round trip", &TestAuditFormatRoundTrip);
    run(L"audit records copy isolated", &TestAuditRecordsCopyIsolated);
    run(L"audit line rejects empty path", &TestAppendAuditLineRejectsEmptyPath);
    run(L"audit line appends readable lines",
        &TestAppendAuditLineAppendsReadableLines);
    run(L"audit line creates parent dirs",
        &TestAppendAuditLineCreatesParentDirectories);
    run(L"audit line keeps one line per record",
        &TestAppendAuditLineKeepsOneLinePerRecord);
    run(L"audit log persists to file", &TestAuditLogPersistsToFile);
    run(L"audit persistence failure not faked",
        &TestAuditLogPersistenceFailureNotFaked);
    run(L"audit log without path stays in memory",
        &TestAuditLogWithoutFilePathStaysInMemory);
    run(L"audit tail rejects empty path", &TestReadAuditTailRejectsEmptyPath);
    run(L"audit tail missing file is not an error",
        &TestReadAuditTailMissingFileIsNotAnError);
    run(L"audit tail returns last lines", &TestReadAuditTailReturnsLastLines);
    run(L"audit tail zero lines counts only",
        &TestReadAuditTailZeroLinesCountsOnly);
    run(L"audit tail truncates long line",
        &TestReadAuditTailTruncatesLongLine);
    run(L"audit tail directory is failure",
        &TestReadAuditTailDirectoryIsFailure);
    return failed == 0 ? 0 : 1;
}
