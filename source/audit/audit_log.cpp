#include "audit/audit_log.hpp"

#include <sstream>
#include <utility>

namespace optimizer::audit {

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

common::Result<void> AuditLog::Append(AuditRecord record) noexcept {
    if (!options_.enabled) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "AuditLog::Append", L"审计不可用（disabled）"));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.at == std::chrono::steady_clock::time_point{}) {
            record.at = std::chrono::steady_clock::now();
        }
        records_.push_back(std::move(record));
        if (records_.size() > options_.capacity) {
            records_.pop_front(); // 环形：超出容量丢最旧
        }
    }
    return common::Result<void>::Success();
}

bool AuditLog::IsAvailable() const noexcept {
    return options_.enabled;
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

} // namespace optimizer::audit
