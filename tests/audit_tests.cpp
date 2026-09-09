#include "audit/audit_log.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using optimizer::audit::AuditLog;
using optimizer::audit::AuditRecord;
using optimizer::audit::FormatAuditRecord;
using optimizer::audit::RiskLevel;
using optimizer::common::Error;
using optimizer::common::Result;

AuditRecord MakeRecord(const char* op, bool ok = true) {
    AuditRecord r;
    r.operationId = op;
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
    return failed == 0 ? 0 : 1;
}
