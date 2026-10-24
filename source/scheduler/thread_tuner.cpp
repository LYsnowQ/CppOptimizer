#include "scheduler/thread_tuner.hpp"

#include "common/unique_resource.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string>

namespace optimizer::scheduler {

namespace {

[[nodiscard]] common::UniqueHandle OpenThreadHandle(std::uint32_t threadId,
                                                    DWORD access) noexcept {
    return common::UniqueHandle(
        ::OpenThread(access, FALSE, static_cast<DWORD>(threadId)));
}

class RealSchedulerBackendImpl final : public SchedulerBackend {
public:
    common::Result<std::vector<ThreadSnapshot>> ListThreads(
        std::uint32_t processId) override {
        if (processId == 0) {
            return common::Result<std::vector<ThreadSnapshot>>::Failure(
                common::Error::Validation("ListThreads", L"进程 ID 不能为 0"));
        }
        common::UniqueHandle snapshot(
            ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        if (!snapshot) {
            return common::Result<std::vector<ThreadSnapshot>>::Failure(
                common::Error::FromWin32(::GetLastError(), "CreateToolhelp32Snapshot"));
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        std::vector<ThreadSnapshot> threads;
        if (::Thread32First(snapshot.Get(), &entry) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                return common::Result<std::vector<ThreadSnapshot>>::Success({});
            }
            return common::Result<std::vector<ThreadSnapshot>>::Failure(
                common::Error::FromWin32(error, "Thread32First"));
        }
        do {
            if (entry.th32OwnerProcessID != processId) {
                continue;
            }
            ThreadSnapshot item;
            item.threadId = entry.th32ThreadID;
            const auto handle = OpenThreadHandle(entry.th32ThreadID,
                                                 THREAD_QUERY_LIMITED_INFORMATION);
            if (!handle) {
                continue; // 读不到的线程跳过（不伪造 0 值）
            }
            const int priority = ::GetThreadPriority(handle.Get());
            if (priority != THREAD_PRIORITY_ERROR_RETURN) {
                item.priority = priority;
            }
            GROUP_AFFINITY group{};
            if (::GetThreadGroupAffinity(handle.Get(), &group) != FALSE) {
                item.affinityMask = group.Mask;
            }
            threads.push_back(item);
        } while (::Thread32Next(snapshot.Get(), &entry) != FALSE);
        return common::Result<std::vector<ThreadSnapshot>>::Success(
            std::move(threads));
    }

    common::Result<int> GetPriority(std::uint32_t threadId) override {
        const auto handle = OpenThreadHandle(threadId, THREAD_QUERY_LIMITED_INFORMATION);
        if (!handle) {
            return common::Result<int>::Failure(
                common::Error::FromWin32(::GetLastError(), "OpenThread(query)"));
        }
        const int priority = ::GetThreadPriority(handle.Get());
        if (priority == THREAD_PRIORITY_ERROR_RETURN) {
            return common::Result<int>::Failure(
                common::Error::FromWin32(::GetLastError(), "GetThreadPriority"));
        }
        return common::Result<int>::Success(priority);
    }

    common::Result<void> SetPriority(std::uint32_t threadId, int priority) override {
        const auto handle = OpenThreadHandle(threadId, THREAD_SET_INFORMATION);
        if (!handle) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(::GetLastError(), "OpenThread(set)"));
        }
        if (::SetThreadPriority(handle.Get(), priority) == FALSE) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(::GetLastError(), "SetThreadPriority"));
        }
        return common::Result<void>::Success();
    }

    common::Result<std::uint64_t> GetAffinity(std::uint32_t threadId) override {
        const auto handle = OpenThreadHandle(threadId, THREAD_QUERY_LIMITED_INFORMATION);
        if (!handle) {
            return common::Result<std::uint64_t>::Failure(
                common::Error::FromWin32(::GetLastError(), "OpenThread(query)"));
        }
        GROUP_AFFINITY group{};
        if (::GetThreadGroupAffinity(handle.Get(), &group) == FALSE) {
            return common::Result<std::uint64_t>::Failure(
                common::Error::FromWin32(::GetLastError(), "GetThreadGroupAffinity"));
        }
        return common::Result<std::uint64_t>::Success(group.Mask);
    }

    common::Result<void> SetAffinity(std::uint32_t threadId,
                                     std::uint64_t mask) override {
        const auto handle = OpenThreadHandle(threadId, THREAD_SET_INFORMATION);
        if (!handle) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(::GetLastError(), "OpenThread(set)"));
        }
        GROUP_AFFINITY group{};
        group.Mask = static_cast<KAFFINITY>(mask);
        if (::SetThreadGroupAffinity(handle.Get(), &group, nullptr) == FALSE) {
            return common::Result<void>::Failure(
                common::Error::FromWin32(::GetLastError(), "SetThreadGroupAffinity"));
        }
        return common::Result<void>::Success();
    }
};

} // namespace

SchedulerBackend& RealSchedulerBackend() noexcept {
    static RealSchedulerBackendImpl backend;
    return backend;
}

bool IsValidPriority(int priority) noexcept {
    return priority >= -15 && priority <= 15;
}

bool ShouldRaisePriority(int current, int target) noexcept {
    return current < target;
}

common::Result<BoostReport> BoostProcessThreads(
    SchedulerBackend& backend, std::uint32_t processId, int targetPriority) {
    if (!IsValidPriority(targetPriority)) {
        return common::Result<BoostReport>::Failure(common::Error::Validation(
            "BoostProcessThreads", L"目标优先级超出 -15..15"));
    }
    const auto threads = backend.ListThreads(processId);
    if (!threads) {
        return common::Result<BoostReport>::Failure(threads.ErrorValue());
    }
    BoostReport report;
    for (const auto& thread : threads.Value()) {
        if (!ShouldRaisePriority(thread.priority, targetPriority)) {
            ++report.skipped;
            continue;
        }
        const auto applied =
            backend.SetPriority(thread.threadId, targetPriority);
        if (!applied) {
            // 失败即停：已完成的调整如实带回，由调用方决定是否恢复。
            return common::Result<BoostReport>::Success(std::move(report));
        }
        PriorityAdjustment adjustment;
        adjustment.threadId = thread.threadId;
        adjustment.originalPriority = thread.priority;
        adjustment.appliedPriority = targetPriority;
        report.adjusted.push_back(adjustment);
    }
    return common::Result<BoostReport>::Success(std::move(report));
}

common::Result<std::size_t> RestorePriorities(
    SchedulerBackend& backend,
    const std::vector<PriorityAdjustment>& adjustments) {
    std::size_t restored = 0;
    for (const auto& adjustment : adjustments) {
        const auto current = backend.GetPriority(adjustment.threadId);
        if (!current) {
            continue; // 线程已退出或读不到：跳过，不覆盖
        }
        if (current.Value() != adjustment.appliedPriority) {
            continue; // 外部已改过：不覆盖他人修改
        }
        if (!backend.SetPriority(adjustment.threadId, adjustment.originalPriority)) {
            return common::Result<std::size_t>::Failure(
                common::Error::FromWin32(::GetLastError(), "RestorePriorities"));
        }
        ++restored;
    }
    return common::Result<std::size_t>::Success(restored);
}

std::string FormatAdjustmentRecord(
    std::uint32_t processId,
    const std::vector<PriorityAdjustment>& adjustments) {
    std::ostringstream out;
    out << "CppOptimizerSchedulerRecord/1\n";
    out << "pid=" << processId << " count=" << adjustments.size() << "\n";
    for (const auto& item : adjustments) {
        out << "tid=" << item.threadId << " from=" << item.originalPriority
            << " to=" << item.appliedPriority << "\n";
    }
    return out.str();
}

common::Result<std::vector<PriorityAdjustment>> ParseAdjustmentRecord(
    std::string_view text) {
    std::vector<PriorityAdjustment> adjustments;
    std::istringstream in{std::string(text)};
    std::string line;
    if (!std::getline(in, line) || line != "CppOptimizerSchedulerRecord/1") {
        return common::Result<std::vector<PriorityAdjustment>>::Failure(
            common::Error::Validation("ParseAdjustmentRecord", L"信封行不符合契约"));
    }
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.rfind("pid=", 0) == 0) {
            continue; // 头部统计行（pid/count），不参与逐条解析
        }
        PriorityAdjustment item;
        const std::string prefixTid = "tid=";
        const std::string prefixFrom = " from=";
        const std::string prefixTo = " to=";
        if (line.compare(0, prefixTid.size(), prefixTid) != 0) {
            return common::Result<std::vector<PriorityAdjustment>>::Failure(
                common::Error::Validation("ParseAdjustmentRecord", L"行前缀不合法"));
        }
        const auto fromPos = line.find(prefixFrom);
        const auto toPos = line.find(prefixTo);
        if (fromPos == std::string::npos || toPos == std::string::npos ||
            toPos < fromPos) {
            return common::Result<std::vector<PriorityAdjustment>>::Failure(
                common::Error::Validation("ParseAdjustmentRecord", L"缺字段"));
        }
        const auto tidText = line.substr(prefixTid.size(), fromPos - prefixTid.size());
        const auto fromText = line.substr(fromPos + prefixFrom.size(),
                                         toPos - fromPos - prefixFrom.size());
        const auto toText = line.substr(toPos + prefixTo.size());
        unsigned long tid = 0;
        int from = 0;
        int to = 0;
        if (std::from_chars(tidText.data(), tidText.data() + tidText.size(), tid).ec !=
                std::errc() ||
            std::from_chars(fromText.data(), fromText.data() + fromText.size(), from).ec !=
                std::errc() ||
            std::from_chars(toText.data(), toText.data() + toText.size(), to).ec !=
                std::errc()) {
            return common::Result<std::vector<PriorityAdjustment>>::Failure(
                common::Error::Validation("ParseAdjustmentRecord", L"数值解析失败"));
        }
        item.threadId = static_cast<std::uint32_t>(tid);
        item.originalPriority = from;
        item.appliedPriority = to;
        adjustments.push_back(item);
    }
    return common::Result<std::vector<PriorityAdjustment>>::Success(
        std::move(adjustments));
}

} // namespace optimizer::scheduler
