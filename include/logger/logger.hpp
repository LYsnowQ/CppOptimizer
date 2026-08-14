#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace optimizer::logger {

// Structured log severity. Filtering rule: a record is written when
// record.level >= Logger::level (default Info), so Trace/Debug are dropped by
// default and Critical is always kept (subject to the configured threshold).
enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5
};

// Stable display names. LevelToString never fails; LevelFromString accepts the
// exact names produced by LevelToString and returns Validation otherwise.
[[nodiscard]] const wchar_t* LevelToString(LogLevel level) noexcept;
[[nodiscard]] common::Result<LogLevel> LevelFromString(std::wstring_view name) noexcept;

// A single structured log line. localTime is expected in the exact display
// format produced by the logger ("YYYY-MM-DD HH:MM:SS.mmm"); keeping it a plain
// field makes FormatLogRecord a pure function (no system calls, no I/O), so it
// can be unit-tested with fixed inputs and reused by any sink.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::wstring localTime;
    std::wstring module;
    std::wstring message;
};

// Pure formatting: "<localTime> [<LEVEL>] <module>: <message>". No I/O, no
// allocation policy beyond the returned string. May allocate.
[[nodiscard]] std::wstring FormatLogRecord(const LogRecord& record);

// Thread-safe logger backed by spdlog (compiled mode). This is a thin adapter:
// it keeps the project's public contract (LogLevel / Result / UTF-8 wide text)
// and maps failures onto common::Error instead of spdlog exceptions, so logging
// can never take the caller down. Sink failures degrade to the debug output.
class Logger {
public:
    explicit Logger(LogLevel level = LogLevel::Info) noexcept;
    ~Logger() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void SetLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel Level() const noexcept;

    // Opens (or creates) a log file in append mode and switches the sink to it.
    // On failure the logger stays on the debug sink and returns a Win32-style
    // error; it never throws and never leaves a half-open sink behind.
    [[nodiscard]] common::Result<void> SetFileSink(const std::wstring& path) noexcept;

    // Switches the sink to stderr or to the debug output (OutputDebugStringW).
    void SetStderrSink() noexcept;
    void SetDebugSink() noexcept;

    // Writes one record when record.level >= Level(). Never throws: spdlog
    // exceptions are caught and degraded, so logging cannot take the caller
    // down (log failure must not recurse into another log write).
    void Write(LogLevel level, std::wstring_view module,
               std::wstring_view message) noexcept;

private:
    LogLevel level_;
    std::shared_ptr<void> impl_; // opaque spdlog logger (owns sinks + thread safety)
};

} // namespace optimizer::logger
