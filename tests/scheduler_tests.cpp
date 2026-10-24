#include "scheduler/thread_tuner.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using optimizer::common::Error;
using optimizer::common::Result;
using optimizer::scheduler::BoostProcessThreads;
using optimizer::scheduler::FormatAdjustmentRecord;
using optimizer::scheduler::IsValidPriority;
using optimizer::scheduler::ParseAdjustmentRecord;
using optimizer::scheduler::PriorityAdjustment;
using optimizer::scheduler::RestorePriorities;
using optimizer::scheduler::SchedulerBackend;
using optimizer::scheduler::ShouldRaisePriority;
using optimizer::scheduler::ThreadSnapshot;

class FakeBackend final : public SchedulerBackend {
public:
    std::vector<ThreadSnapshot> threads;
    std::vector<std::pair<std::uint32_t, int>> writes;
    bool failWrite = false;

    Result<std::vector<ThreadSnapshot>> ListThreads(std::uint32_t processId) override {
        if (processId == 0) {
            return Result<std::vector<ThreadSnapshot>>::Failure(
                Error::Validation("ListThreads", L"进程 ID 不能为 0"));
        }
        return Result<std::vector<ThreadSnapshot>>::Success(threads);
    }

    Result<int> GetPriority(std::uint32_t threadId) override {
        for (const auto& thread : threads) {
            if (thread.threadId == threadId) {
                return Result<int>::Success(thread.priority);
            }
        }
        return Result<int>::Failure(Error::Validation("GetPriority", L"线程不存在"));
    }

    Result<void> SetPriority(std::uint32_t threadId, int priority) override {
        if (failWrite) {
            return Result<void>::Failure(Error::Validation("SetPriority", L"模拟写入失败"));
        }
        writes.emplace_back(threadId, priority);
        for (auto& thread : threads) {
            if (thread.threadId == threadId) {
                thread.priority = priority;
            }
        }
        return Result<void>::Success();
    }

    Result<std::uint64_t> GetAffinity(std::uint32_t threadId) override {
        (void)threadId;
        return Result<std::uint64_t>::Success(1);
    }

    Result<void> SetAffinity(std::uint32_t threadId, std::uint64_t mask) override {
        (void)threadId;
        (void)mask;
        return Result<void>::Success();
    }
};

bool TestPriorityRange() {
    return IsValidPriority(-15) && IsValidPriority(0) && IsValidPriority(15) &&
           !IsValidPriority(-16) && !IsValidPriority(16);
}

bool TestShouldRaise() {
    return ShouldRaisePriority(0, 1) && !ShouldRaisePriority(1, 1) &&
           !ShouldRaisePriority(2, 1);
}

bool TestBoostOnlyRaisesLowerThreads() {
    FakeBackend backend;
    backend.threads = {{100, 0, 1}, {101, 2, 1}, {102, -2, 1}};
    const auto report = BoostProcessThreads(backend, 42, 1);
    if (!report) {
        return false;
    }
    // 0 -> 1 与 -2 -> 1 被调整；2 不动。
    if (report.Value().adjusted.size() != 2 || report.Value().skipped != 1) {
        return false;
    }
    const auto& first = report.Value().adjusted[0];
    const auto& second = report.Value().adjusted[1];
    return first.threadId == 100 && first.originalPriority == 0 &&
           first.appliedPriority == 1 && second.threadId == 102 &&
           second.originalPriority == -2 && second.appliedPriority == 1;
}

bool TestBoostRejectsInvalidPriority() {
    FakeBackend backend;
    backend.threads = {{100, 0, 1}};
    return !BoostProcessThreads(backend, 42, 99);
}

bool TestBoostStopsOnWriteFailureButKeepsApplied() {
    FakeBackend backend;
    backend.threads = {{100, 0, 1}, {101, 0, 1}};
    backend.failWrite = true;
    const auto report = BoostProcessThreads(backend, 42, 1);
    // 写失败：不抛错，但已完成的调整如实返回（此处 0 条）。
    return report && report.Value().adjusted.empty();
}

bool TestRestoreIsConditional() {
    FakeBackend backend;
    backend.threads = {{100, 1, 1}, {101, 2, 1}}; // 101 被外部改成 2
    std::vector<PriorityAdjustment> adjustments = {{100, 0, 1}, {101, 0, 1}};
    const auto restored = RestorePriorities(backend, adjustments);
    if (!restored || restored.Value() != 1) {
        return false;
    }
    return backend.threads[0].priority == 0 && backend.threads[1].priority == 2;
}

bool TestRecordRoundTrip() {
    std::vector<PriorityAdjustment> adjustments = {{100, 0, 1}, {101, -2, 1}};
    const auto text = FormatAdjustmentRecord(42, adjustments);
    const auto parsed = ParseAdjustmentRecord(text);
    if (!parsed || parsed.Value().size() != 2) {
        return false;
    }
    return parsed.Value()[0].threadId == 100 &&
           parsed.Value()[0].originalPriority == 0 &&
           parsed.Value()[1].threadId == 101 &&
           parsed.Value()[1].originalPriority == -2;
}

bool TestRecordRejectsBadEnvelope() {
    return !ParseAdjustmentRecord("Bad/1\n");
}

bool TestRecordRejectsBadLine() {
    return !ParseAdjustmentRecord("CppOptimizerSchedulerRecord/1\nxxx\n");
}

int g_failures = 0;

void Run(const wchar_t* name, bool (*test)()) {
    if (test()) {
        std::wcout << L"PASS " << name << L"\n";
        return;
    }
    ++g_failures;
    std::wcout << L"FAIL " << name << L"\n";
}

} // namespace

int wmain() {
    Run(L"priority range", &TestPriorityRange);
    Run(L"should raise", &TestShouldRaise);
    Run(L"boost only raises lower", &TestBoostOnlyRaisesLowerThreads);
    Run(L"boost rejects invalid priority", &TestBoostRejectsInvalidPriority);
    Run(L"boost stops on write failure", &TestBoostStopsOnWriteFailureButKeepsApplied);
    Run(L"restore is conditional", &TestRestoreIsConditional);
    Run(L"record round trip", &TestRecordRoundTrip);
    Run(L"record rejects bad envelope", &TestRecordRejectsBadEnvelope);
    Run(L"record rejects bad line", &TestRecordRejectsBadLine);
    if (g_failures != 0) {
        std::wcout << g_failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"all scheduler tests passed\n";
    return 0;
}
