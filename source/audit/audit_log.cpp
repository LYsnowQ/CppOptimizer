#include "audit/audit_log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace optimizer::audit {

namespace {

// 本地时间戳（ASCII "YYYY-MM-DD HH:MM:SS"）。localtime_s 失败返回空串（行仍可写）。
std::string LocalTimestampAscii() noexcept {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local{};
    if (::localtime_s(&local, &now) != 0) {
        return "";
    }
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) ==
        0) {
        return "";
    }
    return std::string(buffer);
}

// 等级名（如 "R1"）为 ASCII：逐宽字符收窄直写（与 UTF-8 文件兼容，避免窄化告警）。
std::string NarrowAscii(const wchar_t* text) {
    std::string out;
    for (const wchar_t* p = text; p != nullptr && *p != L'\0'; ++p) {
        out.push_back(static_cast<char>(*p));
    }
    return out;
}

// 单行不变量：字段内的控制字符（含换行/回车/Tab）改写为空格，避免一条记录跨行。
void AppendSanitized(std::string& out, const std::string& field) {
    for (const unsigned char ch : field) {
        out.push_back(ch < 0x20 || ch == 0x7F ? ' ' : static_cast<char>(ch));
    }
}

// 从汇总行的字段段中读取 `<key><digits>`（key 形如 "total="）。缺失/非数字/溢出返回 false：
// “无法判定”不得当作 0（回看时少报比报错更危险）。
bool ExtractCount(std::string_view fields, std::string_view key,
                  std::size_t& out) noexcept {
    const auto at = fields.find(key);
    if (at == std::string_view::npos) {
        return false;
    }
    std::size_t value = 0;
    std::size_t digits = 0;
    for (std::size_t i = at + key.size(); i < fields.size(); ++i) {
        const char ch = fields[i];
        if (ch < '0' || ch > '9') {
            break;
        }
        if (value > (std::numeric_limits<std::size_t>::max() - 9) / 10) {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(ch - '0');
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    out = value;
    return true;
}

// 回看上限：文件超此值拒绝读取（避免无界内存/IO），单行超长截断。
constexpr std::uintmax_t kMaxAuditReadBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxAuditLineBytes = 4096;

// 与 FormatAuditRecord 字段顺序一致的单行文本（无前导/尾随空白）。
std::string FormatAuditLineAscii(const AuditRecord& record) {
    std::string line;
    line += LocalTimestampAscii();
    line += " [audit] ";
    line += NarrowAscii(RiskLevelToString(record.risk));
    line += ' ';
    AppendSanitized(line, record.operationId);
    line += record.ok ? " ok" : " fail";
    line += " caller=";
    AppendSanitized(line, record.caller);
    line += " target=";
    AppendSanitized(line, record.target);
    line += " detail=";
    AppendSanitized(line, record.detail);
    return line;
}

} // namespace

const wchar_t* RiskLevelToString(RiskLevel level) noexcept {
    switch (level) {
        case RiskLevel::R0:
            return L"R0";
        case RiskLevel::R1:
            return L"R1";
        case RiskLevel::R2:
            return L"R2";
        case RiskLevel::R3:
            return L"R3";
        case RiskLevel::R4:
            return L"R4";
    }
    return L"R?";
}

std::wstring FormatAuditRecord(const AuditRecord& record) {
    std::wostringstream line;
    line << L"[audit] " << RiskLevelToString(record.risk) << L" "
         << std::wstring(record.operationId.begin(), record.operationId.end())
         << L" " << (record.ok ? L"ok" : L"fail") << L" caller="
         << std::wstring(record.caller.begin(), record.caller.end())
         << L" target="
         << std::wstring(record.target.begin(), record.target.end())
         << L" detail="
         << std::wstring(record.detail.begin(), record.detail.end());
    return line.str();
}

AuditLog::AuditLog(Options options)
    : options_(options) {}

AuditSummary SummarizeRecords(std::span<const AuditRecord> records) {
    AuditSummary summary;
    bool hasTime = false;
    for (const auto& record : records) {
        ++summary.total;
        if (record.ok) {
            ++summary.ok;
        } else {
            ++summary.fail;
        }
        // 按 operationId 首次出现顺序聚合（记录量小：线性查找足够且稳定）。
        AuditOperationCount* entry = nullptr;
        for (auto& candidate : summary.byOperation) {
            if (candidate.operationId == record.operationId) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            summary.byOperation.push_back(AuditOperationCount{record.operationId, 0, 0});
            entry = &summary.byOperation.back();
        }
        if (record.ok) {
            ++entry->ok;
        } else {
            ++entry->fail;
        }
        // 首/末时刻：跳过未设置的时刻（默认构造的 time_point）。
        if (record.at != std::chrono::steady_clock::time_point{}) {
            if (!hasTime || record.at < summary.firstAt) {
                summary.firstAt = record.at;
            }
            if (!hasTime || record.at > summary.lastAt) {
                summary.lastAt = record.at;
            }
            hasTime = true;
        }
    }
    return summary;
}

std::optional<AuditLinePrefix> ParseAuditLinePrefix(
    std::string_view line) noexcept {
    constexpr std::string_view kMarker = " [audit] ";
    const auto marker = line.find(kMarker);
    if (marker == std::string_view::npos || marker == 0) {
        return std::nullopt;
    }
    std::string_view rest = line.substr(marker + kMarker.size());
    // rest: "<risk> <operationId> <ok|fail> caller=…"
    const auto afterRisk = rest.find(' ');
    if (afterRisk == std::string_view::npos) {
        return std::nullopt;
    }
    rest.remove_prefix(afterRisk + 1);
    const auto afterOperation = rest.find(' ');
    if (afterOperation == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view operationId = rest.substr(0, afterOperation);
    rest.remove_prefix(afterOperation + 1);
    const auto afterResult = rest.find(' ');
    const std::string_view result =
        afterResult == std::string_view::npos ? rest : rest.substr(0, afterResult);
    if (operationId.empty()) {
        return std::nullopt;
    }
    AuditLinePrefix prefix;
    prefix.timestamp = std::string(line.substr(0, marker));
    prefix.operationId = std::string(operationId);
    if (result == "ok") {
        prefix.ok = true;
    } else if (result == "fail") {
        prefix.ok = false;
    } else {
        return std::nullopt; // 结果字段不是 ok/fail：视为不可解析
    }
    return prefix;
}

AuditLineSummary SummarizeAuditLines(const std::vector<std::string>& lines) {
    AuditLineSummary summary;
    for (const auto& line : lines) {
        const auto prefix = ParseAuditLinePrefix(line);
        if (!prefix) {
            ++summary.unparsed; // 不可解析行计数，原文由调用方保留
            continue;
        }
        ++summary.total;
        if (prefix->ok) {
            ++summary.ok;
        } else {
            ++summary.fail;
        }
        AuditOperationCount* entry = nullptr;
        for (auto& candidate : summary.byOperation) {
            if (candidate.operationId == prefix->operationId) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            summary.byOperation.push_back(
                AuditOperationCount{prefix->operationId, 0, 0});
            entry = &summary.byOperation.back();
        }
        if (prefix->ok) {
            ++entry->ok;
        } else {
            ++entry->fail;
        }
        // 文件为追加式：首行/末行的可读取时间戳即时间范围。
        if (summary.firstTimestamp.empty()) {
            summary.firstTimestamp = prefix->timestamp;
        }
        summary.lastTimestamp = prefix->timestamp;
    }
    return summary;
}

bool ShouldCompactAuditFile(std::uintmax_t currentBytes,
                            std::size_t currentLines,
                            const CompactOptions& options) noexcept {
    const bool bytesReached =
        options.maxBytes > 0 && currentBytes >= options.maxBytes;
    const bool linesReached =
        options.maxLines > 0 && currentLines >= options.maxLines;
    return bytesReached || linesReached; // 维度为 0 = 不参与；两者都 0 = 永不触发
}

std::string FormatAuditSummaryLine(const AuditLineSummary& summary) {
    std::string line = summary.firstTimestamp;
    if (!summary.firstTimestamp.empty()) {
        line += "..";
        line += summary.lastTimestamp;
    }
    line += " [audit-summary] total=";
    line += std::to_string(summary.total);
    line += " ok=";
    line += std::to_string(summary.ok);
    line += " fail=";
    line += std::to_string(summary.fail);
    line += " unparsed=";
    line += std::to_string(summary.unparsed);
    for (const auto& entry : summary.byOperation) {
        line += " ";
        line += entry.operationId;
        line += "=";
        line += std::to_string(entry.ok);
        line += "/";
        line += std::to_string(entry.fail);
    }
    return line;
}

common::Result<void> AppendAuditLine(const std::filesystem::path& path,
                                     const AuditRecord& record) noexcept {
    if (path.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "AppendAuditLine", L"路径不能为空"));
    }
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(ec.value()),
                "create_directories(audit log)"));
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open audit log for append"));
    }
    out << FormatAuditLineAscii(record) << "\n";
    out.flush();
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "flush audit log"));
    }
    return common::Result<void>::Success();
}

common::Result<void> AuditLog::Append(AuditRecord record) noexcept {
    if (!options_.enabled) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "AuditLog::Append", L"审计不可用（disabled）"));
    }
    if (record.at == std::chrono::steady_clock::time_point{}) {
        record.at = std::chrono::steady_clock::now();
    }
    if (options_.filePath.empty()) {
        // 仅进程内（AUD-001 行为）：无外部副作用。
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(std::move(record));
        if (records_.size() > options_.capacity) {
            records_.pop_front(); // 环形：超出容量丢最旧
        }
        return common::Result<void>::Success();
    }
    // 持久化模式：先落盘再入内存；落盘失败如实返回且不入内存，故「成功」即已持久化。
    // 锁内完成，避免同一 AuditLog 的多线程把同一文件的行写交错。
    std::lock_guard<std::mutex> lock(mutex_);
    auto appended = AppendAuditLine(options_.filePath, record);
    if (!appended) {
        return appended;
    }
    records_.push_back(std::move(record));
    if (records_.size() > options_.capacity) {
        records_.pop_front();
    }
    return common::Result<void>::Success();
}

bool AuditLog::IsAvailable() const noexcept {
    return options_.enabled;
}

bool AuditLog::IsPersistent() const noexcept {
    return !options_.filePath.empty();
}

std::vector<AuditRecord> AuditLog::Records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<AuditRecord>(records_.begin(), records_.end());
}

std::size_t AuditLog::Capacity() const noexcept {
    return options_.capacity;
}

std::size_t AuditLog::Size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

common::Result<AuditTail> ReadAuditTail(const std::filesystem::path& path,
                                        std::size_t maxLines) noexcept {
    if (path.empty()) {
        return common::Result<AuditTail>::Failure(common::Error::Validation(
            "ReadAuditTail", L"路径不能为空"));
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return common::Result<AuditTail>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(ec.value()), "exists(audit log)"));
    }
    AuditTail tail;
    if (!exists) {
        return common::Result<AuditTail>::Success(std::move(tail)); // 尚无记录
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        // 目录当文件等情形：如实失败，不当作「无记录」。
        return common::Result<AuditTail>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(ec.value()), "file_size(audit log)"));
    }
    if (size > kMaxAuditReadBytes) {
        return common::Result<AuditTail>::Failure(common::Error::Validation(
            "ReadAuditTail", L"审计文件超出可回看上限（8 MiB）"));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return common::Result<AuditTail>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open audit log for read"));
    }
    // 流式读取 + 定长环形：内存只与 maxLines 相关，与文件大小无关。
    std::deque<std::string> ring;
    std::string line;
    while (std::getline(in, line)) {
        ++tail.totalLines;
        if (line.size() > kMaxAuditLineBytes) {
            line.resize(kMaxAuditLineBytes);
            line += "...";
        }
        if (maxLines == 0) {
            continue; // 只计数
        }
        ring.push_back(line);
        if (ring.size() > maxLines) {
            ring.pop_front();
        }
    }
    if (in.bad()) {
        return common::Result<AuditTail>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "read audit log"));
    }
    tail.lines.assign(ring.begin(), ring.end());
    return common::Result<AuditTail>::Success(std::move(tail));
}

std::optional<AuditSummaryRecord> ParseAuditSummaryLine(
    std::string_view line) noexcept {
    constexpr std::string_view kMarker = " [audit-summary] ";
    const auto markerAt = line.find(kMarker);
    if (markerAt == std::string_view::npos) {
        return std::nullopt; // 非汇总行
    }
    AuditSummaryRecord record;
    const std::string_view range = line.substr(0, markerAt);
    if (!range.empty()) {
        const auto dots = range.find("..");
        if (dots == std::string_view::npos) {
            return std::nullopt; // 时间范围形式非法（既非空也非 first..last）
        }
        record.firstTimestamp = std::string(range.substr(0, dots));
        record.lastTimestamp = std::string(range.substr(dots + 2));
        if (record.firstTimestamp.empty() || record.lastTimestamp.empty()) {
            return std::nullopt;
        }
    }
    const std::string_view fields = line.substr(markerAt + kMarker.size());
    if (!ExtractCount(fields, "total=", record.total) ||
        !ExtractCount(fields, "ok=", record.ok) ||
        !ExtractCount(fields, "fail=", record.fail) ||
        !ExtractCount(fields, "unparsed=", record.unparsed)) {
        return std::nullopt; // 缺字段：整体视为不可解析，不按局部值静默部分采纳
    }
    return record;
}

AuditCompactionTotals AnalyzeAuditSummaryLines(
    const std::vector<std::string>& lines) {
    AuditCompactionTotals totals;
    for (const auto& line : lines) {
        const auto record = ParseAuditSummaryLine(line);
        if (!record) {
            ++totals.unparsable; // 如实计入，不让回看少报
            continue;
        }
        ++totals.ranges;
        totals.total += record->total;
        totals.ok += record->ok;
        totals.fail += record->fail;
        totals.unparsed += record->unparsed;
        if (totals.firstTimestamp.empty()) {
            totals.firstTimestamp = record->firstTimestamp;
        }
        if (!record->lastTimestamp.empty()) {
            totals.lastTimestamp = record->lastTimestamp;
        }
    }
    return totals;
}

std::filesystem::path AuditSummaryPath(const std::filesystem::path& path) {
    std::wstring name = path.stem().wstring();
    name += L"-summary";
    name += path.extension().wstring();
    return path.parent_path() / name;
}

common::Result<CompactResult> CompactAuditFile(
    const std::filesystem::path& path, const CompactOptions& options) noexcept {    if (path.empty()) {
        return common::Result<CompactResult>::Failure(
            common::Error::Validation("CompactAuditFile", L"路径不能为空"));
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return common::Result<CompactResult>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(ec.value()), "exists(audit log)"));
    }
    CompactResult result;
    if (!exists) {
        return common::Result<CompactResult>::Success(std::move(result)); // 尚无记录
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        // 目录当文件等情形：如实失败，不当作“无可压缩内容”。
        return common::Result<CompactResult>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(ec.value()), "file_size(audit log)"));
    }
    if (size > kMaxAuditReadBytes) {
        return common::Result<CompactResult>::Failure(common::Error::Validation(
            "CompactAuditFile", L"审计文件超出可压缩上限（8 MiB）"));
    }
    std::vector<std::string> lines;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return common::Result<CompactResult>::Failure(
                common::Error::FromWin32(
                    static_cast<std::uint32_t>(::GetLastError()),
                    "open audit log for compact"));
        }
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(std::move(line));
            line.clear();
        }
        if (in.bad()) {
            return common::Result<CompactResult>::Failure(
                common::Error::FromWin32(
                    static_cast<std::uint32_t>(::GetLastError()),
                    "read audit log for compact"));
        }
    }
    result.beforeLines = lines.size();
    result.afterLines = lines.size();
    if (!ShouldCompactAuditFile(size, lines.size(), options)) {
        return common::Result<CompactResult>::Success(std::move(result)); // 未达阈值
    }
    const std::size_t keep = std::min(options.keepTailLines, lines.size());
    const std::size_t headCount = lines.size() - keep;
    const auto headEnd =
        lines.begin() + static_cast<std::ptrdiff_t>(headCount);
    const std::vector<std::string> head(lines.begin(), headEnd);
    const AuditLineSummary summary = SummarizeAuditLines(head);
    // 保留 = 被压缩段内的不可解析行（原样保留，相对顺序不变）+ 未压缩尾部。
    std::vector<std::string> retained;
    retained.reserve(head.size() + keep + 1);
    for (const auto& line : head) {
        if (!ParseAuditLinePrefix(line)) {
            retained.push_back(line);
        }
    }
    retained.insert(retained.end(), headEnd, lines.end());
    if (retained == lines) {
        result.unparsedKept = summary.unparsed;
        return common::Result<CompactResult>::Success(std::move(result));
    }
    // 自审计记录与前缀重写合并为一次原子替换：不留下“已压缩但自审计缺失”的半完成状态。
    AuditRecord selfRecord;
    selfRecord.operationId = "audit.compact";
    selfRecord.risk = RiskLevel::R0;
    selfRecord.caller = "audit";
    selfRecord.target = path.stem().string() + path.extension().string();
    selfRecord.ok = true;
    selfRecord.detail = "before=" + std::to_string(result.beforeLines) +
                        " after=" + std::to_string(retained.size() + 1) +
                        " summarized=" + std::to_string(summary.total) +
                        " unparsed=" + std::to_string(summary.unparsed) +
                        " threshold_bytes=" + std::to_string(options.maxBytes) +
                        " threshold_lines=" + std::to_string(options.maxLines);
    retained.push_back(FormatAuditLineAscii(selfRecord));
    const std::filesystem::path summaryPath = AuditSummaryPath(path);    // 顺序：先写临时文件 -> 追加汇总行 -> 原子替换。任一步失败都清理临时文件并保持原文件不变；
    // 若替换失败，把汇总文件回滚到追加前的大小（避免重复调用造成重复计数）。
    const std::wstring pathText = path.wstring();
    const std::wstring tempText = pathText + L".tmp";
    {
        std::ofstream out(tempText, std::ios::binary | std::ios::trunc);
        if (!out) {
            return common::Result<CompactResult>::Failure(
                common::Error::FromWin32(
                    static_cast<std::uint32_t>(::GetLastError()),
                    "create audit temp file"));
        }
        for (const auto& kept : retained) {
            out << kept << '\n';
        }
        out.flush();
        if (!out) {
            const auto code = static_cast<std::uint32_t>(::GetLastError());
            out.close();
            ::DeleteFileW(tempText.c_str());
            return common::Result<CompactResult>::Failure(
                common::Error::FromWin32(code, "write audit temp file"));
        }
    }
    std::error_code summaryEc;
    std::uintmax_t summaryBeforeBytes = 0;
    if (std::filesystem::exists(summaryPath, summaryEc)) {
        summaryBeforeBytes = std::filesystem::file_size(summaryPath, summaryEc);
        if (summaryEc) {
            summaryBeforeBytes = 0; // 无法判定原大小：不做回滚，仅如实失败
        }
    }
    {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(summaryPath.parent_path(), ec);
            if (ec) {
                ::DeleteFileW(tempText.c_str());
                return common::Result<CompactResult>::Failure(
                    common::Error::FromWin32(static_cast<std::uint32_t>(ec.value()),
                                             "create_directories(audit summary)"));
            }
        }
        std::ofstream out(summaryPath, std::ios::binary | std::ios::app);
        if (!out) {
            ::DeleteFileW(tempText.c_str());
            return common::Result<CompactResult>::Failure(
                common::Error::FromWin32(
                    static_cast<std::uint32_t>(::GetLastError()),
                    "open audit summary for append"));
        }
        out << FormatAuditSummaryLine(summary) << '\n';
        out.flush();
        if (!out) {
            const auto code = static_cast<std::uint32_t>(::GetLastError());
            ::DeleteFileW(tempText.c_str());
            return common::Result<CompactResult>::Failure(
                common::Error::FromWin32(code, "append audit summary"));
        }
    }
    if (!::MoveFileExW(tempText.c_str(), pathText.c_str(),
                       MOVEFILE_REPLACE_EXISTING)) {
        const auto code = static_cast<std::uint32_t>(::GetLastError());
        ::DeleteFileW(tempText.c_str());
        std::error_code rollbackEc;
        std::filesystem::resize_file(summaryPath, summaryBeforeBytes, rollbackEc);
        return common::Result<CompactResult>::Failure(
            common::Error::FromWin32(code, "MoveFileExW(audit compact)"));
    }
    result.compacted = true;
    result.afterLines = retained.size();
    result.summarizedLines = summary.total;
    result.unparsedKept = summary.unparsed;
    result.summaryPath = summaryPath;
    return common::Result<CompactResult>::Success(std::move(result));
}

} // namespace optimizer::audit
