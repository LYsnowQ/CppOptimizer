#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace optimizer::audit {

// 动作风险等级（审计标注用，与危险操作分级口径一致：R0 只读 / R1 局部可逆 / R2 系统级可逆 /
// R3 系统级实验 / R4 内核或不可接受）。审计模块自身不判定门禁，只记录动作声明等级。
enum class RiskLevel { R0, R1, R2, R3, R4 };

// 等级名（纯查询，恒成功）。
[[nodiscard]] const wchar_t* RiskLevelToString(RiskLevel level) noexcept;

// 一次受审计动作（审计记录字段，AUD-001）：
// operationId 动作标识（如 "priority.boost"/"power.release"）、target 目标描述、detail 参数/
// 结果说明、ok 是否成功、caller 来源（policy/console/recovery 等）、at 发生时刻（单调时钟，
// 仅用于排序与窗口内展示，不跨重启作绝对时间）。
struct AuditRecord {
    std::string operationId;
    RiskLevel risk = RiskLevel::R1;
    std::string caller;
    std::string target;
    std::string detail;
    bool ok = true;
    std::chrono::steady_clock::time_point at{};
};

// 可读格式化（控制台/日志展示用；字段顺序固定便于 grep）。
[[nodiscard]] std::wstring FormatAuditRecord(const AuditRecord& record);

// 有界审计日志（AUD-001）：进程内顺序保存最近 capacity 条受审计动作记录，供窗口汇总/诊断与
// 后续“审计不可用”Safe Mode 触发源使用（真实持久化/服务形态属后续切片）。
// 线程安全（内部互斥）。Append 在 enabled=false 时返回 Failure（审计不可用语义，不伪装成功）。
class AuditLog {
public:
    struct Options {
        std::size_t capacity = 128; // 保留条数上限（环形：超出丢最旧）
        bool enabled = true;        // false = 审计不可用（Append 返回 Failure）
    };

    explicit AuditLog(Options options = {});

    // 追加一条受审计动作记录。审计不可用（disabled）返回 Failure。
    [[nodiscard]] common::Result<void> Append(AuditRecord record) noexcept;

    // 当前是否可用（enabled）。
    [[nodiscard]] bool IsAvailable() const noexcept;

    // 拷贝全部记录（插入顺序，最旧在前）。
    [[nodiscard]] std::vector<AuditRecord> Records() const;

    [[nodiscard]] std::size_t Capacity() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;

private:
    Options options_;
    mutable std::mutex mutex_;
    std::deque<AuditRecord> records_;
};

} // namespace optimizer::audit
