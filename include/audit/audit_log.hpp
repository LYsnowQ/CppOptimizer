#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
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

// 单条审计记录落盘（AUD-002）：以 UTF-8 追加一行到 path——本地时间戳（ASCII
// `YYYY-MM-DD HH:MM:SS`）+ 与 FormatAuditRecord 一致的字段顺序（risk/operationId/ok|fail/
// caller/target/detail）。父目录自动创建；空路径拒绝；打开/写/flush 失败如实返回 Failure
// （不伪装记录成功）。字段内的控制字符（含换行/回车）改写为空格，保证一行一条的不变量。
// 文件为追加式，不做轮转也不删除；同一路径的并发追加需调用方串行化。
[[nodiscard]] common::Result<void> AppendAuditLine(
    const std::filesystem::path& path, const AuditRecord& record) noexcept;

// 审计文件回看结果：totalLines = 文件中可读行总数，lines = 最后 maxLines 行（最旧在前）。
struct AuditTail {
    std::size_t totalLines = 0;
    std::vector<std::string> lines;
};

// 只读回看审计文件末尾（AUD-003）：不写、不截断、不重命名、不删除任何文件。
// 文件不存在 = “尚无审计记录” -> Success（totalLines 0、lines 空），不算错误；空路径返回
// Validation；目录当文件或读取失败如实返回 Failure（不把读取失败伪装成“无记录”）。
// maxLines 为返回行数上限（0 = 只计数不返回内容）；单行超 4096 字节截断并追加 “...”；
// 文件超 8 MiB 以 Validation 拒绝（避免无界读取）。
[[nodiscard]] common::Result<AuditTail> ReadAuditTail(
    const std::filesystem::path& path, std::size_t maxLines) noexcept;

// 有界审计日志（AUD-001）：进程内顺序保存最近 capacity 条受审计动作记录，供窗口汇总/诊断与
// 后续“审计不可用”Safe Mode 触发源使用（真实持久化/服务形态属后续切片）。
// 线程安全（内部互斥）。Append 在 enabled=false 时返回 Failure（审计不可用语义，不伪装成功）。
class AuditLog {
public:
    struct Options {
        std::size_t capacity = 128; // 保留条数上限（环形：超出丢最旧）
        bool enabled = true;        // false = 审计不可用（Append 返回 Failure）
        // 非空 = 审计同时持久化到该文件（一行一条）；空 = 仅进程内（AUD-001 行为）。
        // 持久化模式先落盘再入内存：落盘失败返回 Failure 且不入内存，故 Append 成功
        // 即表示已持久化。文件追加式（不轮转/不删），环形容量只作用于进程内视图。
        std::filesystem::path filePath{};
    };

    explicit AuditLog(Options options = {});

    // 追加一条受审计动作记录。审计不可用（disabled）返回 Failure。
    [[nodiscard]] common::Result<void> Append(AuditRecord record) noexcept;

    // 当前是否可用（enabled）。
    [[nodiscard]] bool IsAvailable() const noexcept;

    // 是否配置了持久化文件（Options.filePath 非空）。
    [[nodiscard]] bool IsPersistent() const noexcept;

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
