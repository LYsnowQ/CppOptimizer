#pragma once

#include "common/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// 审计汇总（AUD-004 第一步，**纯函数**）：按 operationId 聚合成功/失败计数，并给出总数与首/末时刻。
// 用途：语义压缩（压缩而非删除）——把旧记录汇总为一行，保留计数与时间范围，丢弃逐条细节。
// 契约：不分配文件资源、不抛异常；`byOperation` 按 operationId **首次出现顺序**稳定排列；
// 空输入返回全零（firstAt/lastAt 为默认时刻）。
struct AuditOperationCount {
    std::string operationId;
    std::size_t ok = 0;
    std::size_t fail = 0;
};

struct AuditSummary {
    std::size_t total = 0;
    std::size_t ok = 0;
    std::size_t fail = 0;
    std::vector<AuditOperationCount> byOperation;
    std::chrono::steady_clock::time_point firstAt{};
    std::chrono::steady_clock::time_point lastAt{};
};

[[nodiscard]] AuditSummary SummarizeRecords(
    std::span<const AuditRecord> records);

// 审计行前缀解析（AUD-004，纯函数）：从 `AppendAuditLine` 写入的一行中取出时间戳文本、
// operationId 与 ok/fail（只取前 5 个字段，其余视为可丢弃的细节）。
// **畸形/缺字段行返回 nullopt**——调用方不得因此静默丢弃原文。
struct AuditLinePrefix {
    std::string timestamp; // "YYYY-MM-DD HH:MM:SS"（行首文本）
    std::string operationId;
    bool ok = true;
};
[[nodiscard]] std::optional<AuditLinePrefix> ParseAuditLinePrefix(
    std::string_view line) noexcept;

// 审计行聚合（AUD-004，纯函数）：语义压缩的计数基础——按 operationId **首次出现顺序**聚合，
// 保留总数与首/末时间戳；**不可解析行计入 `unparsed`（不得丢弃）**。
struct AuditLineSummary {
    std::size_t total = 0; // 参与聚合的可解析行数
    std::size_t ok = 0;
    std::size_t fail = 0;
    std::size_t unparsed = 0;
    std::vector<AuditOperationCount> byOperation;
    std::string firstTimestamp;
    std::string lastTimestamp;
};
[[nodiscard]] AuditLineSummary SummarizeAuditLines(
    const std::vector<std::string>& lines);

// 汇总行格式化（AUD-004，纯函数）：把行聚合结果渲染为**一行**（写入 audit-summary.log 的形式）：
// `<firstTs>..<lastTs> [audit-summary] total=N ok=N fail=N unparsed=N <op>=<ok>/<fail> …`
// 计数与时间范围为**必须保留**的可追溯信息；逐条细节不进入汇总行。
// 空时间戳时省略 `..` 区间（仍输出计数）。
[[nodiscard]] std::string FormatAuditSummaryLine(const AuditLineSummary& summary);

// 压缩选项（AUD-004）：任一阈值**达到或超过**即应压缩；某项为 0 表示该维度不参与判定，
// 两者都为 0 时永不触发。`keepTailLines` 指定重写后至少保留的尾部行数（未被压缩的部分，
// 含不可解析行——它们必须原样保留）。
struct CompactOptions {
    std::uintmax_t maxBytes = 4u * 1024u * 1024u; // 4 MiB
    std::size_t maxLines = 20000;
    std::size_t keepTailLines = 2000;
};

[[nodiscard]] bool ShouldCompactAuditFile(std::uintmax_t currentBytes,
                                          std::size_t currentLines,
                                          const CompactOptions& options) noexcept;

// 汇总行解析（AUD-004 纯函数）：从 `audit-summary.log` 的一行中取出时间范围与四项计数。
// 不含汇总标记、缺少任一必需字段/字段非数字时返回 nullopt——调用方不得把“无法判定”当作 0。
struct AuditSummaryRecord {
    std::string firstTimestamp;
    std::string lastTimestamp;
    std::size_t total = 0;
    std::size_t ok = 0;
    std::size_t fail = 0;
    std::size_t unparsed = 0;
};
[[nodiscard]] std::optional<AuditSummaryRecord> ParseAuditSummaryLine(
    std::string_view line) noexcept;

// 汇总文件聚合（AUD-004 纯函数）：跨多行汇总给出“被压缩总量 + 整体时间范围”，并把非汇总/畸形行
// 计入 `unparsable`（不得静默丢弃，否则回看会少报）。`firstTimestamp`/`lastTimestamp` 取参与聚合
// 的汇总行的首/末时间点（按文件顺序，不重新排序）。
struct AuditCompactionTotals {
    std::size_t ranges = 0;
    std::size_t total = 0;
    std::size_t ok = 0;
    std::size_t fail = 0;
    std::size_t unparsed = 0;
    std::size_t unparsable = 0; // 未被计入计数的行（非汇总行或字段残缺）
    std::string firstTimestamp;
    std::string lastTimestamp;
};
[[nodiscard]] AuditCompactionTotals AnalyzeAuditSummaryLines(
    const std::vector<std::string>& lines);

// 汇总文件路径（AUD-004）：与审计文件**同目录**的 `<stem>-summary<ext>`（`CompactAuditFile` 的
// 追加目标，也是“压缩不删除”后计数的所在）。纯路径推导，不访问文件系统。
[[nodiscard]] std::filesystem::path AuditSummaryPath(
    const std::filesystem::path& path);

// 压缩结果（AUD-004）：`compacted=false` 表示未达阈值或无内容可压缩（文件未被触碰，
// 重复调用无事发生）；`afterLines` 含压缩后追加的自审计行。
struct CompactResult {
    bool compacted = false;
    std::size_t beforeLines = 0;
    std::size_t afterLines = 0;
    std::size_t summarizedLines = 0; // 被汇总的可解析行数（= 汇总行 total）
    std::size_t unparsedKept = 0;    // 原样保留在 audit 文件里的不可解析行数
    std::filesystem::path summaryPath{};
};

// 语义压缩审计文件（AUD-004）：达阈值时把**旧的可解析记录**聚合为**一行汇总**追加到同目录的
// `<stem>-summary<ext>`，并把审计文件重写为「未压缩尾部 + 被压缩段内的不可解析行原样保留」——
// 只丢弃逐条细节，不删除任何计数信息。安全边界：
// - 先读 -> 汇总 -> 追加汇总行 -> 再重写，任一失败返回 Failure 且**不破坏原文件**；
// - 重写走「临时文件 + flush + 原子替换（MoveFileExW）」，失败清理临时文件；
// - 不可解析行绝不压缩丢弃（原文保留在审计文件中）；
// - 未达阈值、文件不存在、或压缩后内容与原文一致（无可压缩内容）时均为 no-op（compacted=false）；
// - 压缩成功后追加一条 `audit.compact` 自审计记录（含阈值与前后行数），追加失败如实返回 Failure。
// 同一路径的并发调用需调用方串行化。
[[nodiscard]] common::Result<CompactResult> CompactAuditFile(
    const std::filesystem::path& path,
    const CompactOptions& options = {}) noexcept;

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
