#include "logger/logger.hpp"

#include <windows.h>

#include <spdlog/include/spdlog/logger.h>
#include <spdlog/include/spdlog/sinks/base_sink.h>
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

// 宽文本转 UTF-8，项目契约为宽文本，spdlog 处理窄文本，在边界转换。
// 有意不加 noexcept：结果分配可能抛 bad_alloc，两个调用点 Write/SetFileSink
// 均有 try/catch 降级；加 noexcept 将导致 terminate 并绕过降级契约。
// flags 传 0 为有意选择：CP_UTF8 对孤立代理对替换为 U+FFFD 而非整条失败。
std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        // 转换失败：正文降级为空，时间/级别/模块仍输出。
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

// 显式映射，不依赖枚举数值一致。
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

// 有意不加 noexcept：std::format 分配宽字符串可能抛 bad_alloc，同 ToUtf8。
std::wstring NowLocalTime() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return std::format(L"{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds);
}

// 控制台双路径 sink：
// - 直连真实控制台 FILE_TYPE_CHAR：WriteConsoleW 宽字符直写，绕过代码页 936；
// - 重定向至文件/管道：WriteFile 写 UTF-8 字节，保持可移植性。
// 目标类型运行期不变，构造时检测一次即可。
class ConsoleOrUtf8Sink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ConsoleOrUtf8Sink(HANDLE handle)
        : handle_(handle),
          isConsole_(::GetFileType(handle) == FILE_TYPE_CHAR) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        const auto data = reinterpret_cast<const char*>(formatted.data());
        const auto size = static_cast<DWORD>(formatted.size());
        if (isConsole_) {
            // 宽字符直写；控制台按行缓冲，无需显式 flush。
            const int needed = ::MultiByteToWideChar(
                CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
            if (needed <= 0) {
                return;
            }
            std::wstring wide(static_cast<std::size_t>(needed), L'\0');
            ::MultiByteToWideChar(
                CP_UTF8, 0, data, static_cast<int>(size),
                wide.data(), needed);
            DWORD written = 0;
            ::WriteConsoleW(handle_, wide.data(),
                            static_cast<DWORD>(wide.size()), &written, nullptr);
        } else {
            // 重定向：UTF-8 字节原样写出。
            DWORD written = 0;
            ::WriteFile(handle_, data, size, &written, nullptr);
        }
    }

    void flush_() override {
        if (!isConsole_) {
            ::FlushFileBuffers(handle_);
        }
    }

private:
    HANDLE handle_;
    bool isConsole_;
};

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
    // 默认 sink 为 Debug 输出；构造失败时 impl_ 置空。
    try {
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v"); // 原始消息；适配器已预格式化整行
        impl_ = std::move(logger);
    } catch (...) {
        impl_ = nullptr;
    }
}

Logger::~Logger() noexcept {
    // make_shared 直接构造不会注册进 spdlog 全局 registry，注册走 spdlog::create，
    // 因此释放 impl_ 即释放对 sink 的最后引用，文件句柄由 sink 析构关闭。
    impl_.reset();
}

void Logger::SetLevel(LogLevel level) noexcept {
    level_ = level;
    if (impl_) {
        impl_->set_level(ToSpdlogLevel(level_));
    }
}

LogLevel Logger::Level() const noexcept {
    return level_;
}

common::Result<void> Logger::SetFileSink(const std::wstring& path) noexcept {
    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            ToUtf8(path), false /* 追加模式 */);
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v");
        impl_ = std::move(logger);
        return common::Result<void>::Success();
    } catch (const spdlog::spdlog_ex& exception) {
        // 打开失败：保持原 sink。spdlog 不携带打开失败的 Win32 错误码，
        // 尽力读取 GetLastError 并保留异常消息。
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
        // 双路径 sink：真实控制台 -> WriteConsoleW 宽字符绕过代码页；重定向 -> UTF-8。
        auto sink = std::make_shared<ConsoleOrUtf8Sink>(
            ::GetStdHandle(STD_ERROR_HANDLE));
        auto logger = std::make_shared<spdlog::logger>("optimizer", sink);
        logger->set_level(ToSpdlogLevel(level_));
        logger->set_pattern("%v");
        impl_ = std::move(logger);
    } catch (...) {
        // 静默降级：保持当前 sink。
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
        // 静默降级。
    }
}

void Logger::Write(LogLevel level, std::wstring_view module,
                   std::wstring_view message) noexcept {
    if (level < level_) {
        return;
    }
    auto* logger = impl_.get();
    if (logger == nullptr) {
        return; // 构造失败，无可用 sink
    }
    try {
        const std::wstring line =
            FormatLogRecord(LogRecord{level, NowLocalTime(),
                                      std::wstring(module), std::wstring(message)});
        logger->log(ToSpdlogLevel(level), ToUtf8(line));
        // 契约：写入后立即刷盘，确保调用方返回时记录可见。
        logger->flush();
    } catch (...) {
        // sink 写入失败不得使调用方崩溃，亦不得递归触发新的日志写入。
    }
}

} // namespace optimizer::logger
