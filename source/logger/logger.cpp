#include "logger/logger.hpp"

#include "common/unique_resource.hpp"

#include <windows.h>

#include <format>
#include <string>
#include <utility>

namespace optimizer::logger {

namespace {

// Encodes a wide line as UTF-8 and writes it to a handle. Returns false on any
// failure. Used for file and stderr sinks; the debug sink writes wide text
// directly via OutputDebugStringW.
bool WriteUtf8ToHandle(HANDLE handle, std::wstring_view line) noexcept {
    if (line.empty()) {
        return true;
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return false;
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
            utf8.data(), needed, nullptr, nullptr) <= 0) {
        return false;
    }
    DWORD written = 0;
    return ::WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()),
                       &written, nullptr) != FALSE &&
           written == static_cast<DWORD>(utf8.size());
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

Logger::Logger(LogLevel level) noexcept : level_(level) {}

void Logger::SetLevel(LogLevel level) noexcept {
    level_ = level;
}

LogLevel Logger::Level() const noexcept {
    return level_;
}

common::Result<void> Logger::SetFileSink(const std::wstring& path) noexcept {
    // Open outside the lock: CreateFileW may block on slow media; the lock is
    // only held for the handle swap so concurrent Write calls never see a
    // half-configured sink. On failure the logger stays on the debug sink.
    common::UniqueHandle file{::CreateFileW(
        path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file.IsValid()) {
        return common::Result<void>::Failure(
            common::Error::FromWin32(::GetLastError(), "Logger::SetFileSink"));
    }
    std::lock_guard lock(mutex_);
    file_ = std::move(file);
    sink_ = Sink::File;
    return common::Result<void>::Success();
}

void Logger::SetStderrSink() noexcept {
    std::lock_guard lock(mutex_);
    stderrHandle_ = ::GetStdHandle(STD_ERROR_HANDLE);
    sink_ = Sink::Stderr;
}

void Logger::SetDebugSink() noexcept {
    std::lock_guard lock(mutex_);
    sink_ = Sink::Debug;
}

void Logger::Write(LogLevel level, std::wstring_view module,
                   std::wstring_view message) noexcept {
    if (level < level_) {
        return;
    }
    // Formatting outside the lock: FormatLogRecord may allocate; the lock only
    // guards the sink state and the actual write.
    const std::wstring line =
        FormatLogRecord(LogRecord{level, NowLocalTime(),
                                  std::wstring(module), std::wstring(message)});
    std::lock_guard lock(mutex_);
    WriteLine(line);
}

void Logger::WriteLine(std::wstring_view line) noexcept {
    // Sink failure degrades to the debug output and never recurses into
    // another log write: OutputDebugStringW is called directly, so a failing
    // file/stderr sink cannot trigger an unbounded log loop.
    switch (sink_) {
    case Sink::Debug:
        ::OutputDebugStringW(std::wstring(line).c_str());
        break;
    case Sink::File:
        if (file_.IsValid() &&
            WriteUtf8ToHandle(file_.Get(), line) &&
            WriteUtf8ToHandle(file_.Get(), L"\r\n")) {
            break;
        }
        ::OutputDebugStringW(std::wstring(line).c_str());
        break;
    case Sink::Stderr:
        if (stderrHandle_ != nullptr &&
            stderrHandle_ != INVALID_HANDLE_VALUE &&
            WriteUtf8ToHandle(stderrHandle_, line) &&
            WriteUtf8ToHandle(stderrHandle_, L"\r\n")) {
            break;
        }
        ::OutputDebugStringW(std::wstring(line).c_str());
        break;
    }
}

} // namespace optimizer::logger
