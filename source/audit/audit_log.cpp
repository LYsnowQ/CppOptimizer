#include "audit/audit_log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

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

} // namespace optimizer::audit
