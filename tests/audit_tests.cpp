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

// ---------- AUD-004：审计汇总（纯函数，语义压缩的计数基础） ----------

bool TestSummarizeRecordsEmptyIsZero() {
    const auto summary = optimizer::audit::SummarizeRecords({});
    return summary.total == 0 && summary.ok == 0 && summary.fail == 0 &&
           summary.byOperation.empty();
}

bool TestSummarizeRecordsAggregatesByOperation() {
    using optimizer::audit::AuditRecord;
    using optimizer::audit::SummarizeRecords;
    AuditRecord first = MakeRecord("priority.boost", true);
    AuditRecord second = MakeRecord("power.hold", true);
    AuditRecord third = MakeRecord("priority.boost", false);
    const auto base = std::chrono::steady_clock::now();
    first.at = base;
    second.at = base + std::chrono::seconds(2);
    third.at = base + std::chrono::seconds(5);
    const std::vector<AuditRecord> records{first, second, third};

    const auto summary = SummarizeRecords(records);
    if (summary.total != 3 || summary.ok != 2 || summary.fail != 1) {
        return false;
    }
    // 按 operationId 首次出现顺序聚合。
    if (summary.byOperation.size() != 2 ||
        summary.byOperation[0].operationId != "priority.boost" ||
        summary.byOperation[0].ok != 1 || summary.byOperation[0].fail != 1 ||
        summary.byOperation[1].operationId != "power.hold" ||
        summary.byOperation[1].ok != 1 || summary.byOperation[1].fail != 0) {
        return false;
    }
    // 时间范围保留（语义压缩不得丢失可追溯信息）。
    return summary.firstAt == base &&
           summary.lastAt == base + std::chrono::seconds(5);
}

// ---------- AUD-004：审计行前缀解析与行聚合（纯函数） ----------

bool TestParseAuditLinePrefixParsesAndRejects() {
    using optimizer::audit::ParseAuditLinePrefix;
    const std::string okLine =
        "2026-09-17 00:33:17 [audit] R1 power.hold ok caller=policy target=policy detail=x";
    const std::string failLine =
        "2026-09-17 00:33:22 [audit] R1 priority.unboost fail caller=policy";
    const auto ok = ParseAuditLinePrefix(okLine);
    const auto fail = ParseAuditLinePrefix(failLine);
    const bool parsed = ok && fail && ok->timestamp == "2026-09-17 00:33:17" &&
                        ok->operationId == "power.hold" && ok->ok &&
                        fail->operationId == "priority.unboost" && !fail->ok;
    // 畸形/缺字段/结果非 ok|fail：必须返回 nullopt（不静默当作可解析）。
    const bool rejected =
        !ParseAuditLinePrefix("") &&
        !ParseAuditLinePrefix("no marker here") &&
        !ParseAuditLinePrefix("2026-09-17 00:33:17 [audit] R1 onlyRisk") &&
        !ParseAuditLinePrefix("2026-09-17 00:33:17 [audit] R1 op maybe") &&
        !ParseAuditLinePrefix("2026-09-17 00:33:17 [audit] ");
    return parsed && rejected;
}

bool TestSummarizeAuditLinesCountsAndKeepsRange() {
    using optimizer::audit::SummarizeAuditLines;
    const std::vector<std::string> lines{
        "2026-09-17 00:00:01 [audit] R1 power.hold ok caller=policy",
        "2026-09-17 00:00:02 [audit] R1 priority.boost ok caller=policy",
        "2026-09-17 00:00:03 [audit] R1 power.hold fail caller=policy",
        "",                                       // 不可解析（空行）
        "garbage without marker",                 // 不可解析
        "2026-09-17 00:00:04 [audit] R1 power.hold ok caller=policy"};
    const auto summary = SummarizeAuditLines(lines);
    if (summary.total != 4 || summary.ok != 3 || summary.fail != 1 ||
        summary.unparsed != 2) {
        return false;
    }
    // 首次出现顺序：power.hold 先于 priority.boost。
    if (summary.byOperation.size() != 2 ||
        summary.byOperation[0].operationId != "power.hold" ||
        summary.byOperation[0].ok != 2 || summary.byOperation[0].fail != 1 ||
        summary.byOperation[1].operationId != "priority.boost" ||
        summary.byOperation[1].ok != 1) {
        return false;
    }
    return summary.firstTimestamp == "2026-09-17 00:00:01" &&
           summary.lastTimestamp == "2026-09-17 00:00:04";
}

bool TestFormatAuditSummaryLine() {
    using optimizer::audit::AuditLineSummary;
    using optimizer::audit::AuditOperationCount;
    using optimizer::audit::FormatAuditSummaryLine;
    AuditLineSummary summary;
    summary.total = 4;
    summary.ok = 3;
    summary.fail = 1;
    summary.unparsed = 2;
    summary.firstTimestamp = "2026-09-17 00:00:01";
    summary.lastTimestamp = "2026-09-17 00:00:04";
    summary.byOperation = {AuditOperationCount{"power.hold", 2, 1},
                           AuditOperationCount{"priority.boost", 1, 0}};
    const std::string expected =
        "2026-09-17 00:00:01..2026-09-17 00:00:04 [audit-summary] total=4 ok=3 "
        "fail=1 unparsed=2 power.hold=2/1 priority.boost=1/0";
    if (FormatAuditSummaryLine(summary) != expected) {
        return false;
    }
    // 空聚合：无时间戳时省略区间，但仍输出计数（不得输出空串）。
    const AuditLineSummary empty;
    return FormatAuditSummaryLine(empty) ==
           " [audit-summary] total=0 ok=0 fail=0 unparsed=0";
}

bool TestShouldCompactAuditFile() {
    using optimizer::audit::CompactOptions;
    using optimizer::audit::ShouldCompactAuditFile;
    CompactOptions options; // 默认 4 MiB / 20000 行
    // 低于阈值：不触发；达到或超过：触发。
    const bool normal = !ShouldCompactAuditFile(options.maxBytes - 1, 10, options) &&
                        ShouldCompactAuditFile(options.maxBytes, 10, options) &&
                        ShouldCompactAuditFile(0, options.maxLines, options) &&
                        !ShouldCompactAuditFile(0, options.maxLines - 1, options);
    // 维度为 0 = 该维度不参与判定。
    CompactOptions onlyLines;
    onlyLines.maxBytes = 0;
    const bool disabled = !ShouldCompactAuditFile(1u << 30, 1, onlyLines) &&
                          ShouldCompactAuditFile(1, onlyLines.maxLines, onlyLines);
    // 两者都为 0：永不触发（防止误配置导致“每次调用都压缩”）。
    CompactOptions never;
    never.maxBytes = 0;
    never.maxLines = 0;
    const bool neverTriggers =
        !ShouldCompactAuditFile(1ull << 40, 1u << 20, never);
    return normal && disabled && neverTriggers;
}

// ---------- AUD-004：语义压缩（文件级） ----------

std::string ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool TestParseAuditSummaryLine() {
    using optimizer::audit::ParseAuditSummaryLine;
    const std::string line =
        "2026-09-17 00:00:00..2026-09-17 01:00:00 [audit-summary] total=12 "
        "ok=10 fail=2 unparsed=1 priority.boost=10/0";
    const auto record = ParseAuditSummaryLine(line);
    // “无法判定”与“零”不同：非汇总行/缺字段/字段非数字一律 nullopt（不静默部分采纳）。
    const bool rejects = !ParseAuditSummaryLine(
                             "2026-09-17 00:33:17 [audit] R1 op ok") &&
                         !ParseAuditSummaryLine("") &&
                         !ParseAuditSummaryLine(
                             "2026-09-17 00:00:00 [audit-summary] total=1 ok=1") &&
                         !ParseAuditSummaryLine(
                             "2026-09-17 [audit-summary] total=1 ok=1 fail=0 "
                             "unparsed=0") &&
                         !ParseAuditSummaryLine(
                             "2026-09-17 00:00:00..2026-09-17 01:00:00 "
                             "[audit-summary] total=x ok=1 fail=0 unparsed=0");
    // 空时间范围是合法形式（无可用时间戳时仍输出计数），不得当作缺字段拒绝。
    const auto noRange =
        ParseAuditSummaryLine(" [audit-summary] total=3 ok=3 fail=0 unparsed=0");
    return record && record->firstTimestamp == "2026-09-17 00:00:00" &&
           record->lastTimestamp == "2026-09-17 01:00:00" &&
           record->total == 12 && record->ok == 10 && record->fail == 2 &&
           record->unparsed == 1 && noRange && noRange->total == 3 &&
           noRange->firstTimestamp.empty() && rejects;
}

bool TestAnalyzeAuditSummaryLines() {
    using optimizer::audit::AnalyzeAuditSummaryLines;
    const std::vector<std::string> lines = {
        "2026-09-17 00:00:00..2026-09-17 01:00:00 [audit-summary] total=10 "
        "ok=8 fail=2 unparsed=1 power.hold=8/2",
        "malformed summary line",
        "2026-09-18 00:00:00..2026-09-18 00:30:00 [audit-summary] total=5 "
        "ok=5 fail=0 unparsed=0 priority.boost=5/0",
    };
    const auto totals = AnalyzeAuditSummaryLines(lines);
    const bool empty = [] {
        const auto zero = AnalyzeAuditSummaryLines({});
        return zero.ranges == 0 && zero.total == 0 && zero.unparsable == 0 &&
               zero.firstTimestamp.empty() && zero.lastTimestamp.empty();
    }();
    return totals.ranges == 2 && totals.total == 15 && totals.ok == 13 &&
           totals.fail == 2 && totals.unparsed == 1 &&
           totals.unparsable == 1 &&
           totals.firstTimestamp == "2026-09-17 00:00:00" &&
           totals.lastTimestamp == "2026-09-18 00:30:00" && empty;
}

bool TestCompactAuditFileNoopBelowThreshold() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::CompactAuditFile;
    using optimizer::audit::CompactOptions;
    const auto path = TempAuditPath(L"compactnoop");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(optimizer::audit::AuditSummaryPath(path), ec);
    if (!AppendAuditLine(path, MakeRecord("power.hold")) ||
        !AppendAuditLine(path, MakeRecord("power.release"))) {
        return false;
    }
    const auto before = ReadAllBytes(path);
    const auto result = CompactAuditFile(path, CompactOptions{});
    const bool untouched = ReadAllBytes(path) == before &&
                           !std::filesystem::exists(
                               optimizer::audit::AuditSummaryPath(path), ec);
    const bool ok = result && !result.Value().compacted &&
                    result.Value().beforeLines == 2 && untouched;
    std::filesystem::remove(path, ec);
    return ok;
}

bool TestCompactAuditFileRejectsEmptyPath() {
    using optimizer::audit::CompactAuditFile;
    const auto result = CompactAuditFile({});
    return !result && result.ErrorValue().domain ==
                           optimizer::common::ErrorDomain::Validation;
}

bool TestCompactAuditFileSummarizesAndKeepsTail() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactAuditFile;
    using optimizer::audit::CompactOptions;
    using optimizer::audit::ParseAuditSummaryLine;
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"compact");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(path);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(summaryPath, ec);
    for (int i = 1; i <= 5; ++i) {
        if (!AppendAuditLine(path, MakeRecord("op" + std::to_string(i),
                                              i % 2 == 0))) {
            return false;
        }
    }
    CompactOptions options;
    options.maxBytes = 0;    // 只按行数判定
    options.maxLines = 3;    // 达到即触发
    options.keepTailLines = 1;
    const auto compacted = CompactAuditFile(path, options);
    if (!compacted || !compacted.Value().compacted) {
        return false;
    }
    const auto& result = compacted.Value();
    const bool counts = result.beforeLines == 5 && result.afterLines == 2 &&
                        result.summarizedLines == 4 &&
                        result.unparsedKept == 0 &&
                        result.summaryPath == summaryPath;
    // 汇总行：计数与时间范围保留（不可只留“压缩过了”这一事实）。
    const auto summaryTail = ReadAuditTail(summaryPath, 10);
    const auto summaryRecord =
        summaryTail && summaryTail.Value().lines.size() == 1
            ? ParseAuditSummaryLine(summaryTail.Value().lines[0])
            : std::nullopt;
    const bool summaryKeepsCounts =
        summaryRecord && summaryRecord->total == 4 && summaryRecord->ok == 2 &&
        summaryRecord->fail == 2 &&
        summaryRecord->firstTimestamp == summaryRecord->lastTimestamp;
    // 审计文件：仅留未压缩尾部（最后一条记录）+ 自审计行。
    const auto tail = ReadAuditTail(path, 10);
    const bool keptTail = tail && tail.Value().totalLines == 2 &&
                          tail.Value().lines[0].find("op5") != std::string::npos &&
                          tail.Value().lines[1].find("audit.compact") !=
                              std::string::npos;
    // 重复调用：已低于阈值 -> 不再压缩（幂等，不重复计数）。
    const auto again = CompactAuditFile(path, options);
    const bool idempotent = again && !again.Value().compacted;
    const auto summaryTailAgain = ReadAuditTail(summaryPath, 10);
    const bool summaryNotDuplicated =
        summaryTailAgain && summaryTailAgain.Value().totalLines == 1;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(summaryPath, ec);
    return counts && summaryKeepsCounts && keptTail && idempotent &&
           summaryNotDuplicated;
}

bool TestCompactAuditFileKeepsUnparsedLines() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactAuditFile;
    using optimizer::audit::CompactOptions;
    using optimizer::audit::ParseAuditSummaryLine;
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"compactunparsed");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(path);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(summaryPath, ec);
    // 畸形行不得被压缩丢弃：它既不是可汇总的记录，也不得丢失。
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not an audit record\n";
    }
    for (int i = 1; i <= 3; ++i) {
        if (!AppendAuditLine(path, MakeRecord("op" + std::to_string(i)))) {
            return false;
        }
    }
    CompactOptions options;
    options.maxBytes = 0;
    options.maxLines = 2;
    options.keepTailLines = 1;
    const auto compacted = CompactAuditFile(path, options);
    if (!compacted || !compacted.Value().compacted) {
        return false;
    }
    const bool counts = compacted.Value().summarizedLines == 2 &&
                        compacted.Value().unparsedKept == 1;
    const auto tail = ReadAuditTail(path, 10);
    const bool unparsedKept =
        tail && tail.Value().lines.size() == 3 &&
        tail.Value().lines[0] == "not an audit record" &&
        tail.Value().lines[1].find("op3") != std::string::npos;
    const auto summaryTail = ReadAuditTail(summaryPath, 10);
    const auto summaryRecord =
        summaryTail && !summaryTail.Value().lines.empty()
            ? ParseAuditSummaryLine(summaryTail.Value().lines[0])
            : std::nullopt;
    const bool summaryReportsUnparsed = summaryRecord &&
                                        summaryRecord->total == 2 &&
                                        summaryRecord->unparsed == 1;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(summaryPath, ec);
    return counts && unparsedKept && summaryReportsUnparsed;
}

bool TestCompactAuditFileFailureKeepsOriginal() {
    using optimizer::audit::AppendAuditLine;
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactAuditFile;
    using optimizer::audit::CompactOptions;
    const auto path = TempAuditPath(L"compactfail");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(path);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(summaryPath, ec);
    for (int i = 1; i <= 3; ++i) {
        if (!AppendAuditLine(path, MakeRecord("op" + std::to_string(i)))) {
            return false;
        }
    }
    const auto before = ReadAllBytes(path);
    // 用同名目录占位汇总文件路径：追加必然失败 -> 原文件必须字节不变、临时文件必须清理。
    if (!std::filesystem::create_directory(summaryPath, ec) || ec) {
        return false;
    }
    CompactOptions options;
    options.maxBytes = 0;
    options.maxLines = 2;
    options.keepTailLines = 1;
    const auto result = CompactAuditFile(path, options);
    const bool failedHonestly = !result;
    const bool originalKept = ReadAllBytes(path) == before;
    std::error_code tempEc;
    const bool tempCleaned =
        !std::filesystem::exists(path.wstring() + L".tmp", tempEc);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(summaryPath, ec);
    return failedHonestly && originalKept && tempCleaned;
}

// ---------- AUD-005：汇总文件二级折叠（5 MiB 触发 / 20 MiB 上限） ----------

// 汇总行内容为 ASCII，直接用 std::string（写入时按 UTF-8 字节落盘）。
const std::string kSummaryLineA =
    "2026-09-17 00:00:00..2026-09-17 01:00:00 [audit-summary] total=10 "
    "ok=8 fail=2 unparsed=1 power.hold=6/2 priority.boost=2/0\n";
const std::string kSummaryLineB =
    "2026-09-18 00:00:00..2026-09-18 02:00:00 [audit-summary] total=5 "
    "ok=5 fail=0 unparsed=0 power.hold=1/0 power.release=4/0\n";

bool TestParseAuditSummaryLineOperationCounts() {
    using optimizer::audit::ParseAuditSummaryLine;
    const std::string line =
        "2026-09-17 00:00:00..2026-09-17 01:00:00 [audit-summary] total=10 "
        "ok=8 fail=2 unparsed=1 power.hold=6/2 priority.boost=2/0";
    const auto record = ParseAuditSummaryLine(line);
    if (!record || record->byOperation.size() != 2) {
        return false;
    }
    const bool first = record->byOperation[0].operationId == "power.hold" &&
                       record->byOperation[0].ok == 6 &&
                       record->byOperation[0].fail == 2;
    const bool second = record->byOperation[1].operationId == "priority.boost" &&
                        record->byOperation[1].ok == 2 &&
                        record->byOperation[1].fail == 0;
    // 无尾部字段合法（旧行/空聚合）；尾部字段畸形则整行不可解析。
    const auto plain = ParseAuditSummaryLine(
        " [audit-summary] total=3 ok=3 fail=0 unparsed=0");
    const bool plainOk = plain && plain->byOperation.empty();
    const bool rejects =
        !ParseAuditSummaryLine(
            " [audit-summary] total=3 ok=3 fail=0 unparsed=0 power.hold=1") &&
        !ParseAuditSummaryLine(
            " [audit-summary] total=3 ok=3 fail=0 unparsed=0 =1/0") &&
        !ParseAuditSummaryLine(
            " [audit-summary] total=3 ok=3 fail=0 unparsed=0 power.hold=1/x");
    return first && second && plainOk && rejects;
}

bool TestFoldAuditSummaryMergesCounts() {
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactOptions;
    using optimizer::audit::FoldAuditSummaryFile;
    using optimizer::audit::ParseAuditSummaryLine;
    using optimizer::audit::ReadAuditTail;
    const auto auditPath = TempAuditPath(L"foldmerge");
    if (auditPath.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(auditPath);
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    {
        std::ofstream out(summaryPath, std::ios::binary | std::ios::trunc);
        out << kSummaryLineA;
        out << kSummaryLineB;
    }
    CompactOptions options;
    options.summaryMaxBytes = 1; // 达上限即折叠（测试用极小阈值）
    const auto folded = FoldAuditSummaryFile(summaryPath, options);
    if (!folded || !folded.Value().folded) {
        std::filesystem::remove(auditPath, ec);
        std::filesystem::remove(summaryPath, ec);
        return false;
    }
    const bool counts = folded.Value().mergedRanges == 2 &&
                        folded.Value().afterLines == 1;
    const auto tail = ReadAuditTail(summaryPath, 10);
    if (!tail || tail.Value().lines.size() != 1) {
        std::filesystem::remove(auditPath, ec);
        std::filesystem::remove(summaryPath, ec);
        return false;
    }
    const auto merged = ParseAuditSummaryLine(tail.Value().lines[0]);
    // 计数求和、时间范围跨两段、逐操作计数合并（power.hold 跨两行相加）。
    bool operationMerged = false;
    if (merged) {
        for (const auto& entry : merged->byOperation) {
            if (entry.operationId == "power.hold" && entry.ok == 7 && entry.fail == 2) {
                operationMerged = true;
            }
        }
    }
    const bool mergedOk = merged && merged->total == 15 && merged->ok == 13 &&
                          merged->fail == 2 && merged->unparsed == 1 &&
                          merged->byOperation.size() == 3 && operationMerged &&
                          merged->firstTimestamp == "2026-09-17 00:00:00" &&
                          merged->lastTimestamp == "2026-09-18 02:00:00";
    // 自审计：折叠事件写入审计 trail（审计文件可能因此被新建）。
    const auto auditTail = ReadAuditTail(auditPath, 10);
    const bool selfAudited =
        auditTail && auditTail.Value().lines.size() == 1 &&
        auditTail.Value().lines[0].find("audit.fold") != std::string::npos &&
        auditTail.Value().lines[0].find("merged_ranges=2") != std::string::npos;
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    return counts && mergedOk && selfAudited;
}

bool TestFoldAuditSummaryNoopBelowCap() {
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactOptions;
    using optimizer::audit::FoldAuditSummaryFile;
    const auto auditPath = TempAuditPath(L"foldnoop");
    if (auditPath.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(auditPath);
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    {
        std::ofstream out(summaryPath, std::ios::binary | std::ios::trunc);
        out << kSummaryLineA;
    }
    const auto before = ReadAllBytes(summaryPath);
    const auto folded = FoldAuditSummaryFile(summaryPath, CompactOptions{});
    const bool untouched = ReadAllBytes(summaryPath) == before &&
                           !std::filesystem::exists(auditPath, ec);
    const bool result = folded && !folded.Value().folded;
    // 上限为 0 = 该维度不参与判定（永不折叠）。
    CompactOptions disabled;
    disabled.summaryMaxBytes = 0;
    const auto skipped = FoldAuditSummaryFile(summaryPath, disabled);
    std::filesystem::remove(summaryPath, ec);
    return result && untouched && skipped && !skipped.Value().folded;
}

bool TestFoldAuditSummaryKeepsUnparsableLines() {
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactOptions;
    using optimizer::audit::FoldAuditSummaryFile;
    using optimizer::audit::ReadAuditTail;
    const auto auditPath = TempAuditPath(L"foldgarbage");
    if (auditPath.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(auditPath);
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    {
        std::ofstream out(summaryPath, std::ios::binary | std::ios::trunc);
        out << "garbage summary line\n";
        out << kSummaryLineA;
        out << kSummaryLineB; // 两行可折叠汇总：才会真正减少行数
    }
    CompactOptions options;
    options.summaryMaxBytes = 1;
    const auto folded = FoldAuditSummaryFile(summaryPath, options);
    const auto tail = ReadAuditTail(summaryPath, 10);
    const bool kept =
        folded && folded.Value().folded && folded.Value().unparsableKept == 1 &&
        tail && tail.Value().lines.size() == 2 &&
        tail.Value().lines[0].find("[audit-summary]") != std::string::npos &&
        tail.Value().lines[1] == "garbage summary line";
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    return kept;
}

bool TestFoldAuditSummaryFailuresAreReported() {
    using optimizer::audit::AuditSummaryPath;
    using optimizer::audit::CompactOptions;
    using optimizer::audit::FoldAuditSummaryFile;
    const auto auditPath = TempAuditPath(L"foldfail");
    if (auditPath.empty()) {
        return false;
    }
    std::error_code ec;
    const auto summaryPath = AuditSummaryPath(auditPath);
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    CompactOptions options;
    options.summaryMaxBytes = 1;
    // 空路径拒绝。
    const auto empty = FoldAuditSummaryFile({}, options);
    // 目录当文件：读失败如实返回。
    if (!std::filesystem::create_directory(summaryPath, ec) || ec) {
        return false;
    }
    const auto directory = FoldAuditSummaryFile(summaryPath, options);
    const bool rejected = !empty &&
                          empty.ErrorValue().domain == optimizer::common::ErrorDomain::Validation &&
                          !directory;
    // 自审计目标不可写（同名目录占位）时：折叠已生效但如实返回失败（契约声明的顺序）。
    std::filesystem::remove(summaryPath, ec);
    {
        std::ofstream out(summaryPath, std::ios::binary | std::ios::trunc);
        out << kSummaryLineA;
        out << kSummaryLineB; // 两行不同内容：折叠会真正改变文件（单行折叠 = no-op）
    }
    if (!std::filesystem::create_directory(auditPath, ec) || ec) {
        return false;
    }
    const auto selfAuditFailed = FoldAuditSummaryFile(summaryPath, options);
    const auto summaryTail = optimizer::audit::ReadAuditTail(summaryPath, 10);
    const bool foldedButReported =
        !selfAuditFailed &&
        selfAuditFailed.ErrorValue().domain == optimizer::common::ErrorDomain::Win32 &&
        summaryTail && summaryTail.Value().totalLines == 1;
    std::filesystem::remove(auditPath, ec);
    std::filesystem::remove(summaryPath, ec);
    return rejected && foldedButReported;
}

// ---------- AUD-007：动作日记（操作前/操作后/操作后状态，独立本地文件） ----------

bool TestJournalPhasesAndFields() {
    using optimizer::audit::AppendJournalLine;
    using optimizer::audit::JournalPhase;
    using optimizer::audit::JournalPhaseToString;
    using optimizer::audit::ReadAuditTail;
    const auto path = TempAuditPath(L"journal");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    // 空路径拒绝（不生成无名文件）。
    const auto empty = AppendJournalLine({}, JournalPhase::Before, "op", true,
                                         "t", "d");
    const bool rejected =
        !empty && empty.ErrorValue().domain ==
                      optimizer::common::ErrorDomain::Validation;
    // 三段各写一条：阶段名与字段可回读。
    const bool appended =
        AppendJournalLine(path, JournalPhase::Before, "agent.form_install", true,
                          "task", "intent: install") &&
        AppendJournalLine(path, JournalPhase::After, "agent.form_install", false,
                          "task", "denied") &&
        AppendJournalLine(path, JournalPhase::State, "agent.form_install", true,
                          "task", "startup_tray=no task=no service=no");
    const auto tail = ReadAuditTail(path, 10);
    if (!appended || !tail || tail.Value().lines.size() != 3) {
        std::filesystem::remove(path, ec);
        return rejected;
    }
    const std::string& first = tail.Value().lines[0];
    const std::string& second = tail.Value().lines[1];
    const std::string& third = tail.Value().lines[2];
    const bool phases = first.find("[journal] before ") != std::string::npos &&
                        second.find("[journal] after ") != std::string::npos &&
                        third.find("[journal] state ") != std::string::npos;
    const bool results = first.find(" ok ") != std::string::npos &&
                         second.find(" fail ") != std::string::npos;
    const bool fields = first.find("target=task") != std::string::npos &&
                        third.find("startup_tray=no task=no service=no") !=
                            std::string::npos;
    const bool names = std::string(JournalPhaseToString(JournalPhase::Before)) ==
                           "before" &&
                       std::string(JournalPhaseToString(JournalPhase::After)) ==
                           "after" &&
                       std::string(JournalPhaseToString(JournalPhase::State)) ==
                           "state";
    std::filesystem::remove(path, ec);
    return rejected && phases && results && fields && names;
}

bool TestJournalFailureIsHonest() {
    using optimizer::audit::AppendJournalLine;
    using optimizer::audit::JournalPhase;
    // 目录当文件：追加失败如实返回（不伪装已记录）。
    const auto path = TempAuditPath(L"journaldir");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (!std::filesystem::create_directory(path, ec) || ec) {
        return false;
    }
    const auto failed =
        AppendJournalLine(path, JournalPhase::Before, "op", true, "t", "d");
    std::filesystem::remove(path, ec);
    return !failed;
}

// ---------- 审计可写探测（门禁用：只打开不写入） ----------

bool TestProbeAuditWritable() {
    using optimizer::audit::ProbeAuditWritable;
    using optimizer::audit::ReadAuditTail;
    // 空路径：Validation 拒绝。
    const auto empty = ProbeAuditWritable({});
    // 正常路径：成功且**不写入任何记录**（文件不存在则被创建/为空）。
    const auto path = TempAuditPath(L"probe");
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto ok = ProbeAuditWritable(path);
    const auto tail = ReadAuditTail(path, 10);
    const bool noRecords = tail && tail.Value().totalLines == 0;
    // 目录当文件：如实失败（不得把“写不进去”当作可用）。
    std::filesystem::remove(path, ec);
    if (!std::filesystem::create_directory(path, ec) || ec) {
        return false;
    }
    const auto directory = ProbeAuditWritable(path);
    std::filesystem::remove(path, ec);
    return !empty &&
           empty.ErrorValue().domain == optimizer::common::ErrorDomain::Validation &&
           ok && noRecords && !directory;
}

bool TestParseJournalLine() {
    using optimizer::audit::ParseJournalLine;
    const auto before = ParseJournalLine(
        "2026-09-23 21:09:23 [journal] before power.hold ok caller=cli target=x "
        "detail=intent");
    const auto after = ParseJournalLine(
        "2026-09-23 21:09:23 [journal] after agent.form_install fail caller=cli "
        "target=task detail=denied");
    if (!before || !after) {
        return false;
    }
    const bool fields = before->timestamp == "2026-09-23 21:09:23" &&
                        before->phase == "before" &&
                        before->operationId == "power.hold" && before->ok &&
                        after->phase == "after" &&
                        after->operationId == "agent.form_install" && !after->ok;
    // 非日记行 / 缺字段 / 结果非 ok|fail：一律 nullopt。
    const bool rejects =
        !ParseJournalLine("2026-09-17 00:33:17 [audit] R1 op ok") &&
        !ParseJournalLine("") &&
        !ParseJournalLine("[journal] before op ok") &&
        !ParseJournalLine(" [journal] before") &&
        !ParseJournalLine(" [journal] before op maybe");
    return fields && rejects;
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
    run(L"summarize records empty is zero", &TestSummarizeRecordsEmptyIsZero);
    run(L"summarize records aggregates by operation",
        &TestSummarizeRecordsAggregatesByOperation);
    run(L"parse audit line prefix parses and rejects",
        &TestParseAuditLinePrefixParsesAndRejects);
    run(L"summarize audit lines counts and keeps range",
        &TestSummarizeAuditLinesCountsAndKeepsRange);
    run(L"format audit summary line", &TestFormatAuditSummaryLine);
    run(L"should compact audit file thresholds", &TestShouldCompactAuditFile);
    run(L"parse audit summary line total", &TestParseAuditSummaryLine);
    run(L"parse audit summary operation counts",
        &TestParseAuditSummaryLineOperationCounts);
    run(L"fold audit summary merges counts", &TestFoldAuditSummaryMergesCounts);
    run(L"fold audit summary noop below cap",
        &TestFoldAuditSummaryNoopBelowCap);
    run(L"fold audit summary keeps unparsable lines",
        &TestFoldAuditSummaryKeepsUnparsableLines);
    run(L"fold audit summary failures are reported",
        &TestFoldAuditSummaryFailuresAreReported);
    run(L"journal phases and fields", &TestJournalPhasesAndFields);
    run(L"journal failure is honest", &TestJournalFailureIsHonest);
    run(L"probe audit writable", &TestProbeAuditWritable);
    run(L"parse journal line", &TestParseJournalLine);
    run(L"analyze audit summary lines", &TestAnalyzeAuditSummaryLines);
    run(L"compact audit file noop below threshold",
        &TestCompactAuditFileNoopBelowThreshold);
    run(L"compact audit file rejects empty path",
        &TestCompactAuditFileRejectsEmptyPath);
    run(L"compact audit file summarizes and keeps tail",
        &TestCompactAuditFileSummarizesAndKeepsTail);
    run(L"compact audit file keeps unparsed lines",
        &TestCompactAuditFileKeepsUnparsedLines);
    run(L"compact audit file failure keeps original",
        &TestCompactAuditFileFailureKeepsOriginal);
    return failed == 0 ? 0 : 1;
}
