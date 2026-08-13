#pragma once

#include "common/error.hpp"
#include "common/unique_resource.hpp"

#include <cstdint>
#include <mutex>
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

// Thread-safe synchronous logger. Owns exactly one optional file handle (RAII:
// closed on destruction). Sink failures degrade to the debug output and never
// recurse into another log write; the logger never throws.
class Logger {
public:
    explicit Logger(LogLevel level = LogLevel::Info) noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void SetLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel Level() const noexcept;

    // Opens (or creates) a log file in append mode and switches the sink to it.
    // On failure the logger stays on the debug sink and returns a Win32 error;
    // it never throws and never leaves a half-open sink behind.
    [[nodiscard]] common::Result<void> SetFileSink(const std::wstring& path) noexcept;

    // Switches the sink to stderr (borrowed handle; never closed by the logger)
    // or to the debug output (OutputDebugStringW). Both are synchronous.
    void SetStderrSink() noexcept;
    void SetDebugSink() noexcept;

    // Writes one record when record.level >= Level(). Never throws: all
    // failures (formatting, sink write, degradation) are swallowed after the
    // degradation path has run, so logging can never take the caller down.
    void Write(LogLevel level, std::wstring_view module,
               std::wstring_view message) noexcept;

private:
    enum class Sink {
        Debug,
        File,
        Stderr
    };

    LogLevel level_;
    std::mutex mutex_;
    Sink sink_ = Sink::Debug;
    common::UniqueHandle file_;
    void* stderrHandle_ = nullptr; // borrowed; never closed

    void WriteLine(std::wstring_view line) noexcept;
};

} // namespace optimizer::logger
