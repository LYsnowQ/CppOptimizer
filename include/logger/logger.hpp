#pragma once

#include "common/error.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>

namespace spdlog {
class logger;
} // namespace spdlog

namespace optimizer::logger {

// 日志级别。过滤规则：level >= Logger 当前级别才写入，默认 Info。
enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5
};

// 级别名转换：ToString 恒成功；FromString 接受 ToString 的输出，**ASCII 大小写不敏感**
// （配置 [logging].level 的口径为小写，如 "info"），其它名称返回 Validation。
[[nodiscard]] const wchar_t* LevelToString(LogLevel level) noexcept;
[[nodiscard]] common::Result<LogLevel> LevelFromString(std::wstring_view name) noexcept;

// 结构化日志行。localTime 须为 logger 产出的 "YYYY-MM-DD HH:MM:SS.mmm" 格式；
// 保持为纯字段使 FormatLogRecord 成为无 I/O 纯函数。
struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::wstring localTime;
    std::wstring module;
    std::wstring message;
};

// 纯格式化： "<localTime> [<LEVEL>] <module>: <message>"。无 I/O，可能分配。
[[nodiscard]] std::wstring FormatLogRecord(const LogRecord& record);

// 日志文件 sink 选项（与配置 [logging] 的 max_file_mb / max_files / console 口径一致）。
// maxFileMb == 0 = 不轮转（单文件持续追加）；> 0 时单文件达上限即轮转。
// maxFiles = **保留的历史文件数上限（不含当前文件）**，磁盘上日志文件总量不超过
// maxFiles + 1（0 视为 1）；超限时删除最旧的历史文件。
// alsoConsole = true 时在文件之外**额外**挂一个 stderr 双路径 sink（多 sink 同时输出）。
struct FileSinkOptions {
    std::uint32_t maxFileMb = 10;
    std::uint32_t maxFiles = 5;
    bool alsoConsole = false;
};

// 基于 spdlog compiled 模式的线程安全日志器薄适配层。契约：
// - 保持项目宽文本(UTF-16)/Result 错误模型，异常映射为 common::Error；
// - 日志失败绝不能让调用方崩溃；
// - sink 失败降级到 Debug 输出。
class AsyncLogQueue; // 见本文件后部定义（有界异步队列）

class Logger {
public:
    explicit Logger(LogLevel level = LogLevel::Info) noexcept;
    ~Logger() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void SetLevel(LogLevel level) noexcept;
    // 纯查询（getter）：无失败、无副作用，丢弃返回值合法，故不加 nodiscard。
    LogLevel Level() const noexcept;

    // 以追加模式打开日志文件并切换 sink。失败时保持原 sink 并返回 Win32 错误，
    // 不抛出、不留下半开 sink。轮转与是否附带控制台副本由 options 控制
    // （见 FileSinkOptions）。**同步模式**：Write 返回时记录已落盘（默认行为）。
    [[nodiscard]] common::Result<void> SetFileSink(
        const std::wstring& path, FileSinkOptions options = {}) noexcept;

    // **异步模式**（显式开启，默认不开）：同一文件 sink 由单写线程消费有界队列写出。
    // 契约差异：Write 返回仅代表“已入队”，**不承诺写后立即可见**；Stop/析构时排空，
    // 排空后才保证全部落盘。队列满且记录为 Error/Critical 时，该条**同步直写**（不丢）；
    // 已 Stop 后继续 Write 同样走同步直写（降级不伪装）。
    [[nodiscard]] common::Result<void> SetAsyncFileSink(
        const std::wstring& path, FileSinkOptions options = {},
        std::size_t queueCapacity = 1024) noexcept;

    // 停止异步写线程并排空队列（幂等；同步模式下为无操作）。析构自动调用。
    void StopAsync() noexcept;

    // 冲刷：异步模式下阻塞至调用时刻已入队的记录全部落盘；同步模式下为无操作
    // （同步写入在 Write 返回时已经刷盘，无需额外动作）。
    void Flush() noexcept;

    // 当前是否处于异步模式。
    [[nodiscard]] bool IsAsync() const noexcept;

    // 切换 sink：stderr 控制台双路径或 Debug 输出 OutputDebugStringW。
    void SetStderrSink() noexcept;
    void SetDebugSink() noexcept;

    // 写入一条记录，level >= 当前级别时。绝不抛出：spdlog 异常被捕获并降级，
    // 日志失败不得递归触发新的日志写入。
    void Write(LogLevel level, std::wstring_view module,
               std::wstring_view message) noexcept;

private:
    void WriteNow(const LogRecord& record) noexcept; // 同步写出（含刷盘）

    LogLevel level_;
    std::shared_ptr<spdlog::logger> impl_; // 持有 sink 与线程安全
    std::unique_ptr<AsyncLogQueue> asyncQueue_; // 异步模式（空 = 同步，默认）
};


// 有界异步日志队列（LOG-005 首切片）：业务线程 Push，单写线程按序取用并调用 Sink。
// 契约：
// - Push 只在入队时短暂持锁（不会因下游写入阻塞业务线程）；
// - Sink 回调在写线程执行，**绝不在内部锁内**（允许 Sink 内部再做慢操作）；
// - 队列满时按保守策略：**优先丢弃最低级别**；Error/Critical 永不丢弃
//   —— 队列满且无法腾位时 Push 返回 Failure，调用方应走同步降级路径（不得静默丢弃）；
// - Stop 幂等：先停止接收、再把已入队记录**排空写出**后结束线程；Stop 后 Push 如实失败。
class AsyncLogQueue {
public:
    using Sink = std::function<void(const LogRecord&)>;

    struct Options {
        std::size_t capacity = 1024; // 队列容量（> 0）
    };

    AsyncLogQueue(Sink sink, Options options = {});
    ~AsyncLogQueue() noexcept;

    AsyncLogQueue(const AsyncLogQueue&) = delete;
    AsyncLogQueue& operator=(const AsyncLogQueue&) = delete;

    // 入队一条记录（成功 = 已接管；Failure = 未入队，调用方需自行处理）。
    [[nodiscard]] common::Result<void> Push(LogRecord record) noexcept;

    // 停止并排空（幂等；阻塞至写线程结束）。
    void Stop() noexcept;

    // 中途冲刷：阻塞至“调用时刻已入队的记录全部写出”为止（不停止接收，可继续 Push）。
    // 以“入队序号水位”判定：只看调用前已入队的记录，不受并发 Push 影响；已停止则立即返回。
    void Flush() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::size_t WrittenCount() const noexcept; // 已交给 Sink 的条数
    [[nodiscard]] std::size_t DroppedCount() const noexcept; // 因队列满被丢弃的条数

private:
    void WorkerLoop() noexcept;
    static bool IsProtectedLevel(LogLevel level) noexcept; // Error/Critical

    Sink sink_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<LogRecord> queue_;
    bool stopping_ = false; // 已停止接收（排空阶段）
    bool finished_ = false; // 写线程已结束
    std::size_t written_ = 0;
    std::size_t dropped_ = 0;
    std::uint64_t pushedSeq_ = 0;  // 已入队记录序号（水位基准）
    std::uint64_t writtenSeq_ = 0; // 已写出记录序号（Flush 追平目标）
    std::thread worker_;
};

} // namespace optimizer::logger
