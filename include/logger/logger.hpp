#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <memory>
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

// 级别名转换：ToString 恒成功；FromString 仅接受 ToString 的输出，否则返回 Validation。
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

// 基于 spdlog compiled 模式的线程安全日志器薄适配层。契约：
// - 保持项目宽文本(UTF-16)/Result 错误模型，异常映射为 common::Error；
// - 日志失败绝不能让调用方崩溃；
// - sink 失败降级到 Debug 输出。
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
    // 不抛出、不留下半开 sink。
    [[nodiscard]] common::Result<void> SetFileSink(const std::wstring& path) noexcept;

    // 切换 sink：stderr 控制台双路径或 Debug 输出 OutputDebugStringW。
    void SetStderrSink() noexcept;
    void SetDebugSink() noexcept;

    // 写入一条记录，level >= 当前级别时。绝不抛出：spdlog 异常被捕获并降级，
    // 日志失败不得递归触发新的日志写入。
    void Write(LogLevel level, std::wstring_view module,
               std::wstring_view message) noexcept;

private:
    LogLevel level_;
    std::shared_ptr<spdlog::logger> impl_; // 持有 sink 与线程安全
};

} // namespace optimizer::logger
