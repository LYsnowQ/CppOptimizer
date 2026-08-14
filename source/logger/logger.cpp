#include "logger/logger.hpp"

#include <windows.h>

#include <spdlog/include/spdlog/logger.h>
#include <spdlog/include/spdlog/sinks/basic_file_sink.h>
#include <spdlog/include/spdlog/sinks/msvc_sink.h>
#include <spdlog/include/spdlog/sinks/stdout_sinks.h>
#include <spdlog/include/spdlog/spdlog.h>

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace optimizer::logger {

namespace {

// spdlog works on narrow UTF-8 text; the project contract is wide text, so the
// adapter converts at the boundary. The formatted line is ASCII-safe (level
// names and module tags) plus the message, so UTF-8 conversion is lossless.
std::string ToUtf8(std::wstring_view text) noexcept {
    if (text.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

// Maps the project LogLevel onto the spdlog level enum. The numeric order
// matches (Trace=0 ... Critical=5), but we keep an explicit mapping so the
// adapter does not rely on enum value coincidence.
spdlog::level::level_enum ToSpdlogLevel(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warn:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    }
    return spdlog::level::info;
}

std::wstring NowLocalTime() noexcept {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return std::format(L"{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds);
}

} // namespace

const wchar_t* LevelToString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return L"TRACE";
    case LogLevel::Debug:
        return L"DEBUG";
    case LogLevel::Info:
        return L"INFO";
    case LogLevel::Warn:
        return L"WARN";
    case LogLevel::Error:
        return L"ERROR";
    case LogLevel::Critical:
        return L"CRITICAL";
    }
    return L"UNKNOWN";
}

common::Result<LogLevel> LevelFromString(std::wstring_view name) noexcept {
    if (name == L"TRACE") {
        return common::Result<LogLevel>::Success(LogLevel::Trace);
    }
    if (name == L"DEBUG") {
        return common::Result<LogLevel>::Success(LogLevel::Debug);
    }
    if (name == L"INFO") {
        return common::Result<LogLevel>::Success(LogLevel::Info);
    }
    if (name == L"WARN") {
        return common::Result<LogLevel>::Success(LogLevel::Warn);
    }
    if (name == L"ERROR") {
        return common::Result<LogLevel>::Success(LogLevel::Error);
    }
    if (name == L"CRITICAL") {
        return common::Result<LogLevel>::Success(LogLevel::Critical);
    }
    return common::Result<LogLevel>::Failure(
        common::Error::Validation(
            "LevelFromString", L"Unknown log level name"));
}

std::wstring FormatLogRecord(const LogRecord& record) {
    return std::format(L"{} [{}] {}: {}", record.localTime,
                       LevelToString(record.level), record.module,
                       record.message);
}

Logger::Logger(LogLevel level) noexcept : level_(level) {
    // Default sink: debug output (OutputDebugStringW). The spdlog logger is
    // created lazily safe: an exception here would leave the object unusable,
    // so any failure keeps the adapter on a null implementation that no-ops.
    try {
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v"); // raw message; adapter pre-formats the line
        impl_ = std::move(logger);
    } catch (...) {
        impl_ = nullptr;
    }
}

Logger::~Logger() noexcept = default;

void Logger::SetLevel(LogLevel level) noexcept {
    level_ = level;
    if (auto* logger = static_cast<spdlog::logger*>(impl_.get())) {
        logger->set_level(ToSpdlogLevel(level_));
    }
}

LogLevel Logger::Level() const noexcept {
    return level_;
}

common::Result<void> Logger::SetFileSink(const std::wstring& path) noexcept {
    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            ToUtf8(path), false /* truncate=false -> append */);
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v");
        impl_ = std::move(logger);
        return common::Result<void>::Success();
    } catch (const spdlog::spdlog_ex& exception) {
        // File open failure (bad path, permissions). The adapter stays on the
        // previous sink; failure must not masquerade as success. spdlog does
        // not carry a Win32 code for open failures, so we read GetLastError
        // (best effort) and keep the message from the exception.
        const std::uint32_t code = static_cast<std::uint32_t>(::GetLastError());
        return common::Result<void>::Failure(common::Error::FromWin32(
            code, std::string("Logger::SetFileSink: ") + exception.what()));
    } catch (...) {
        return common::Result<void>::Failure(common::Error::Validation(
            "Logger::SetFileSink", L"Unexpected exception while opening the log file"));
    }
}

void Logger::SetStderrSink() noexcept {
    try {
        auto sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v");
        impl_ = std::move(logger);
    } catch (...) {
        // Degrade silently: keep whatever sink is active.
    }
}

void Logger::SetDebugSink() noexcept {
    try {
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v");
        impl_ = std::move(logger);
    } catch (...) {
        // Degrade silently.
    }
}

void Logger::Write(LogLevel level, std::wstring_view module,
                   std::wstring_view message) noexcept {
    if (level < level_) {
        return;
    }
    auto* logger = static_cast<spdlog::logger*>(impl_.get());
    if (logger == nullptr) {
        return; // construction failed; nothing to write to
    }
    try {
        // Pre-format the whole line as one pure function result, then hand the
        // narrow UTF-8 text to spdlog. spdlog adds its own eol ("\r\n" on
        // Windows) and pattern is "%v" so no double formatting happens.
        const std::wstring line =
            FormatLogRecord(LogRecord{level, NowLocalTime(),
                                      std::wstring(module), std::wstring(message)});
        logger->log(ToSpdlogLevel(level), ToUtf8(line));
        // Keep the write visible immediately: the hand-written logger wrote
        // unbuffered, so the adapter flushes after each record to preserve that
        // contract (readers observe records as soon as Write returns).
        logger->flush();
    } catch (...) {
        // A failing sink must never take the caller down nor recurse into
        // another log write; swallow after degradation.
    }
}

} // namespace optimizer::logger
