#include "logger/logger.hpp"

#include <windows.h>

#include <spdlog/include/spdlog/logger.h>
#include <spdlog/include/spdlog/sinks/base_sink.h>
#include <spdlog/include/spdlog/sinks/basic_file_sink.h>
#include <spdlog/include/spdlog/sinks/msvc_sink.h>
#include <spdlog/include/spdlog/sinks/rotating_file_sink.h>
#include <spdlog/include/spdlog/sinks/stdout_sinks.h>
#include <spdlog/include/spdlog/spdlog.h>

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

// ASCII 大小写不敏感比较（无分配）：级别名的配置口径为小写（[logging].level = "info"），
// ToString 输出为大写，两者必须都能解析，否则配置里文档化的值无法使用。
namespace {

bool EqualsIgnoreCaseAscii(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        wchar_t left = a[i];
        wchar_t right = b[i];
        if (left >= L'a' && left <= L'z') {
            left = static_cast<wchar_t>(left - L'a' + L'A');
        }
        if (right >= L'a' && right <= L'z') {
            right = static_cast<wchar_t>(right - L'a' + L'A');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
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
    if (EqualsIgnoreCaseAscii(name, L"TRACE")) {
        return common::Result<LogLevel>::Success(LogLevel::Trace);
    }
    if (EqualsIgnoreCaseAscii(name, L"DEBUG")) {
        return common::Result<LogLevel>::Success(LogLevel::Debug);
    }
    if (EqualsIgnoreCaseAscii(name, L"INFO")) {
        return common::Result<LogLevel>::Success(LogLevel::Info);
    }
    if (EqualsIgnoreCaseAscii(name, L"WARN")) {
        return common::Result<LogLevel>::Success(LogLevel::Warn);
    }
    if (EqualsIgnoreCaseAscii(name, L"ERROR")) {
        return common::Result<LogLevel>::Success(LogLevel::Error);
    }
    if (EqualsIgnoreCaseAscii(name, L"CRITICAL")) {
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
    StopAsync(); // 先排空并结束写线程，再释放 sink

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

common::Result<void> Logger::SetFileSink(const std::wstring& path,
                                         FileSinkOptions options) noexcept {
    try {
        std::shared_ptr<spdlog::sinks::sink> fileSink;
        if (options.maxFileMb > 0) {
            // 轮转模式：单文件达 maxFileMb 后轮转；maxFiles 为历史文件数上限（不含当前
            // 文件），超限时删除最旧的历史文件（配置声明的保留语义）。
            const std::size_t maxBytes =
                static_cast<std::size_t>(options.maxFileMb) * 1024u * 1024u;
            const std::size_t maxFiles =
                options.maxFiles > 0 ? options.maxFiles : 1;
            fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                ToUtf8(path), maxBytes, maxFiles, false /* 打开时不轮转 */);
        } else {
            // maxFileMb == 0：显式关闭轮转，单文件持续追加。
            fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                ToUtf8(path), false /* 追加模式 */);
        }
        // 控制台副本为可选附加 sink（[logging].console）：与文件同时输出。
        std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks{fileSink};
        if (options.alsoConsole) {
            sinks.push_back(std::make_shared<ConsoleOrUtf8Sink>(
                ::GetStdHandle(STD_ERROR_HANDLE)));
        }
        auto logger = std::make_shared<spdlog::logger>(
            "optimizer", sinks.begin(), sinks.end());
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

void Logger::WriteNow(const LogRecord& record) noexcept {
    auto* logger = impl_.get();
    if (logger == nullptr) {
        return; // 构造失败，无可用 sink
    }
    try {
        const std::wstring line = FormatLogRecord(record);
        logger->log(ToSpdlogLevel(record.level), ToUtf8(line));
        // 同步契约：写入后立即刷盘，确保调用方返回时记录可见。
        logger->flush();
    } catch (...) {
        // sink 写入失败不得使调用方崩溃，亦不得递归触发新的日志写入。
    }
}

void Logger::Write(LogLevel level, std::wstring_view module,
                   std::wstring_view message) noexcept {
    if (level < level_) {
        return;
    }
    const LogRecord record{level, NowLocalTime(), std::wstring(module),
                           std::wstring(message)};
    if (asyncQueue_) {
        // 异步模式：成功仅代表“已入队”；失败（队列满且保护级别 / 已停止）走同步直写降级，不丢记录。
        const auto queued = asyncQueue_->Push(record);
        if (queued) {
            return;
        }
    }
    WriteNow(record);
}

common::Result<void> Logger::SetAsyncFileSink(const std::wstring& path,
                                              FileSinkOptions options,
                                              std::size_t queueCapacity) noexcept {
    // 先建立同步 sink（异步写线程也写同一个 sink）；失败则保持原状态。
    auto opened = SetFileSink(path, options);
    if (!opened) {
        return opened;
    }
    StopAsync(); // 若已在异步模式：先排空旧队列（幂等）
    try {
        AsyncLogQueue::Options queueOptions;
        queueOptions.capacity = queueCapacity > 0 ? queueCapacity : 1;
        asyncQueue_ = std::make_unique<AsyncLogQueue>(
            [this](const LogRecord& record) { WriteNow(record); }, queueOptions);
    } catch (...) {
        // 线程创建失败：如实失败并保持同步模式（已打开的 sink 仍可用）。
        asyncQueue_.reset();
        return common::Result<void>::Failure(common::Error::Unsupported(
            "Logger::SetAsyncFileSink", L"异步写线程创建失败"));
    }
    return common::Result<void>::Success();
}

void Logger::StopAsync() noexcept {
    if (asyncQueue_) {
        asyncQueue_->Stop(); // 幂等：排空已入队记录后结束写线程
        asyncQueue_.reset();
    }
}

bool Logger::IsAsync() const noexcept {
    return asyncQueue_ != nullptr;
}

void Logger::Flush() noexcept {
    if (asyncQueue_) {
        asyncQueue_->Flush(); // 异步：等到调用时刻已入队的记录全部写出
    }
    // 同步模式：Write 返回时已刷盘（LOG-002 契约），无需额外动作。
}


AsyncLogQueue::AsyncLogQueue(Sink sink, Options options)
    : sink_(std::move(sink)), options_(options) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

AsyncLogQueue::~AsyncLogQueue() noexcept {
    Stop(); // 析构即排空并 join（不依赖进程退出清理）
}

bool AsyncLogQueue::IsProtectedLevel(LogLevel level) noexcept {
    return level == LogLevel::Error || level == LogLevel::Critical;
}

common::Result<void> AsyncLogQueue::Push(LogRecord record) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "AsyncLogQueue::Push", L"队列已停止接收（Stop 后不再入队）"));
    }
    if (queue_.size() < options_.capacity) {
        queue_.push_back(std::move(record));
        ++pushedSeq_;
        cv_.notify_one();
        return common::Result<void>::Success();
    }
    // 队列满：优先丢弃最低级别；Error/Critical 永不丢弃（保护级别交调用方同步降级）。
    if (IsProtectedLevel(record.level)) {
        // 尝试腾位：若队列中存在低于该保护级别的记录，替换最旧的低级别记录。
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (!IsProtectedLevel(it->level) && it->level < record.level) {
                *it = std::move(record);
                ++dropped_; // 被替换者即被丢弃
                ++pushedSeq_; // 该保护级别记录已入队（Flush 需等它写出）
                return common::Result<void>::Success();
            }
        }
        // 无可腾位：如实失败，调用方走同步降级（不得静默丢弃保护级别记录）。
        return common::Result<void>::Failure(common::Error::Unsupported(
            "AsyncLogQueue::Push",
            L"队列已满且无低级别记录可腾位（保护级别不丢弃）"));
    }
    // 非保护级别且队列满：丢弃本条（丢弃策略只作用于低级别）。
    ++dropped_;
    return common::Result<void>::Success();
}

void AsyncLogQueue::Stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && finished_) {
            return; // 幂等：已完成
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AsyncLogQueue::Flush() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t target = pushedSeq_; // 只看调用时刻已入队的记录（不受并发 Push 影响）
    cv_.wait(lock, [this, target] {
        return writtenSeq_ >= target || finished_;
    });
}

void AsyncLogQueue::WorkerLoop() noexcept {
    for (;;) {
        LogRecord record;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                break; // 停止且已排空
            }
            record = std::move(queue_.front());
            queue_.pop_front();
        }
        // Sink 在锁外调用（允许慢操作；Sink 内 Push 不会自锁）。
        if (sink_) {
            sink_(record);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++written_;
            ++writtenSeq_;
            cv_.notify_all(); // 唤醒 Flush 等待者
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    cv_.notify_all(); // 停止后 Flush 立即返回
}

bool AsyncLogQueue::IsRunning() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopping_;
}

std::size_t AsyncLogQueue::WrittenCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_;
}

std::size_t AsyncLogQueue::DroppedCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

} // namespace optimizer::logger
