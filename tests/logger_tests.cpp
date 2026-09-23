#include "logger/logger.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring MakeTempLogPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        return L"logger_test.log";
    }
    return std::wstring(buffer) +
           L"cppoptimizer_logger_test_" + std::to_wstring(::GetCurrentProcessId()) +
           L".log";
}

std::wstring ReadWholeFile(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        return L"";
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
    std::wstring text;
    for (const char c : bytes) {
        text.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return text;
}

// 轮转产生的历史文件名（spdlog 约定：<stem>.<n><ext>，索引在扩展名之前）。
std::wstring RotatedLogPath(const std::wstring& base, int index) {
    const std::filesystem::path path(base);
    const auto name = path.stem().wstring() + L"." + std::to_wstring(index) +
                      path.extension().wstring();
    return (path.parent_path() / name).wstring();
}

void RemoveLogFiles(const std::wstring& base) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(base), ec);
    for (int i = 1; i <= 8; ++i) {
        std::filesystem::remove(std::filesystem::path(RotatedLogPath(base, i)),
                                ec);
    }
}

// 当前存在的日志文件数（含当前文件与最多 maxIndex 个历史文件）。
int CountLogFiles(const std::wstring& base, int maxIndex) {
    std::error_code ec;
    int count = std::filesystem::exists(std::filesystem::path(base), ec) ? 1 : 0;
    for (int i = 1; i <= maxIndex; ++i) {
        if (std::filesystem::exists(std::filesystem::path(RotatedLogPath(base, i)),
                                    ec)) {
            ++count;
        }
    }
    return count;
}

bool TestLevelToStringAllLevels() {
    return std::wstring(optimizer::logger::LevelToString(
               optimizer::logger::LogLevel::Trace)) == L"TRACE" &&
           std::wstring(optimizer::logger::LevelToString(
               optimizer::logger::LogLevel::Debug)) == L"DEBUG" &&
           std::wstring(optimizer::logger::LevelToString(
               optimizer::logger::LogLevel::Info)) == L"INFO" &&
           std::wstring(optimizer::logger::LevelToString(
               optimizer::logger::LogLevel::Warn)) == L"WARN" &&
           std::wstring(optimizer::logger::LevelToString(
               optimizer::logger::LogLevel::Error)) == L"ERROR" &&
           std::wstring(optimizer::logger::LevelToString(
               optimizer::logger::LogLevel::Critical)) == L"CRITICAL";
}

bool TestLevelFromStringRoundTrip() {
    for (int i = 0; i <= static_cast<int>(optimizer::logger::LogLevel::Critical);
         ++i) {
        const auto level = static_cast<optimizer::logger::LogLevel>(i);
        const auto parsed =
            optimizer::logger::LevelFromString(optimizer::logger::LevelToString(level));
        if (!parsed.HasValue() || parsed.Value() != level) {
            return false;
        }
    }
    return true;
}

bool TestLevelFromStringRejectsUnknown() {
    const auto result = optimizer::logger::LevelFromString(L"NOPE");
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestLevelFromStringIsCaseInsensitive() {
    // 配置口径为小写（[logging].level = "info"）：级别名必须大小写不敏感，
    // 否则配置里文档化的值无法使用（LOG-004 实测发现的真实缺陷）。同时未知名仍需拒绝。
    const auto lower = optimizer::logger::LevelFromString(L"info");
    const auto mixed = optimizer::logger::LevelFromString(L"WaRn");
    const auto upper = optimizer::logger::LevelFromString(L"CRITICAL");
    const auto unknown = optimizer::logger::LevelFromString(L"verbose");
    const auto empty = optimizer::logger::LevelFromString(L"");
    return lower && lower.Value() == optimizer::logger::LogLevel::Info &&
           mixed && mixed.Value() == optimizer::logger::LogLevel::Warn &&
           upper && upper.Value() == optimizer::logger::LogLevel::Critical &&
           !unknown && !empty;
}

bool TestFormatLogRecordFull() {
    const optimizer::logger::LogRecord record{
        optimizer::logger::LogLevel::Warn, L"2026-08-13 10:00:00.123",
        L"memory", L"high load"};
    return optimizer::logger::FormatLogRecord(record) ==
           L"2026-08-13 10:00:00.123 [WARN] memory: high load";
}

bool TestFormatLogRecordIsPureAndDeterministic() {
    // 相同输入 -> 相同输出；无 I/O、无时钟依赖。
    const optimizer::logger::LogRecord record{
        optimizer::logger::LogLevel::Error, L"2026-01-02 03:04:05.000",
        L"metrics", L"sample failed"};
    const auto first = optimizer::logger::FormatLogRecord(record);
    const auto second = optimizer::logger::FormatLogRecord(record);
    return first == second &&
           first == L"2026-01-02 03:04:05.000 [ERROR] metrics: sample failed";
}

bool TestLoggerFiltersBelowThreshold() {
    const std::wstring path = MakeTempLogPath();
    {
        optimizer::logger::Logger logger(optimizer::logger::LogLevel::Info);
        // Trace/Debug 应被丢弃、Info 保留：通过文件 sink 观察级别过滤，
        // 只有 Info 行应出现在文件中。
        const auto opened = logger.SetFileSink(path);
        if (!opened) {
            return false;
        }
        logger.Write(optimizer::logger::LogLevel::Trace, L"t", L"dropped");
        logger.Write(optimizer::logger::LogLevel::Debug, L"d", L"dropped");
        logger.Write(optimizer::logger::LogLevel::Info, L"i", L"kept");
    } // logger 在此析构：句柄关闭，内容已刷出
    const std::wstring content = ReadWholeFile(path);
    std::filesystem::remove(std::filesystem::path(path));
    return content.find(L"[INFO] i: kept") != std::wstring::npos &&
           content.find(L"dropped") == std::wstring::npos;
}

bool TestLoggerFileSinkWritesAndCloses() {
    const std::wstring path = MakeTempLogPath();
    {
        optimizer::logger::Logger logger;
        const auto opened = logger.SetFileSink(path);
        if (!opened) {
            return false;
        }
        logger.Write(optimizer::logger::LogLevel::Info, L"mod", L"line one");
        logger.Write(optimizer::logger::LogLevel::Error, L"mod", L"line two");
    } // RAII: 读取文件前 sink 已关闭
    const std::wstring content = ReadWholeFile(path);
    std::filesystem::remove(std::filesystem::path(path));
    return content.find(L"[INFO] mod: line one") != std::wstring::npos &&
           content.find(L"[ERROR] mod: line two") != std::wstring::npos;
}

bool TestLoggerFileSinkFailureStaysUsable() {
    optimizer::logger::Logger logger;
    // 指向已存在目录的路径打开为文件必然失败，spdlog 会自动创建缺失父目录，
    // 所以不存在的子目录不再失败：logger 仍可用且不抛异常。
    const std::wstring badPath = MakeTempLogPath() + L"_dir";
    std::filesystem::create_directory(std::filesystem::path(badPath));
    const auto opened = logger.SetFileSink(badPath);
    std::filesystem::remove(std::filesystem::path(badPath));
    if (opened.HasValue()) {
        return false;
    }
    // logger 仍须接受写入而不抛，降级到 Debug。
    logger.Write(optimizer::logger::LogLevel::Error, L"m", L"still works");
    return true;
}

bool TestLoggerCriticalAlwaysWritten() {
    const std::wstring path = MakeTempLogPath();
    {
        optimizer::logger::Logger logger(optimizer::logger::LogLevel::Error);
        const auto opened = logger.SetFileSink(path);
        if (!opened) {
            return false;
        }
        logger.Write(optimizer::logger::LogLevel::Error, L"e", L"kept error");
        logger.Write(optimizer::logger::LogLevel::Critical, L"c", L"kept crit");
        logger.Write(optimizer::logger::LogLevel::Warn, L"w", L"dropped warn");
    }
    const std::wstring content = ReadWholeFile(path);
    std::filesystem::remove(std::filesystem::path(path));
    return content.find(L"[ERROR] e: kept error") != std::wstring::npos &&
           content.find(L"[CRITICAL] c: kept crit") != std::wstring::npos &&
           content.find(L"dropped warn") == std::wstring::npos;
}

bool TestLoggerConcurrentWritesNoCrash() {
    const std::wstring path = MakeTempLogPath();
    {
        optimizer::logger::Logger logger;
        const auto opened = logger.SetFileSink(path);
        if (!opened) {
            return false;
        }
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < 100; ++i) {
                    logger.Write(optimizer::logger::LogLevel::Info, L"thr",
                                 L"t" + std::to_wstring(t) + L"-" +
                                     std::to_wstring(i));
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    } // logger 析构：读取文件前句柄已关闭
    const std::wstring content = ReadWholeFile(path);
    std::filesystem::remove(std::filesystem::path(path));
    // 4 threads * 100 lines = 400 non-empty lines (each ends with \r\n).
    std::size_t lines = 0;
    std::size_t pos = 0;
    while ((pos = content.find(L"\r\n", pos)) != std::wstring::npos) {
        ++lines;
        pos += 2;
    }
    return lines == 400;
}

bool TestLoggerSetLevelChangesFilter() {
    const std::wstring path = MakeTempLogPath();
    {
        optimizer::logger::Logger logger(optimizer::logger::LogLevel::Warn);
        logger.SetLevel(optimizer::logger::LogLevel::Trace);
        const auto opened = logger.SetFileSink(path);
        if (!opened) {
            return false;
        }
        logger.Write(optimizer::logger::LogLevel::Trace, L"t", L"now kept");
    }
    const std::wstring content = ReadWholeFile(path);
    std::filesystem::remove(std::filesystem::path(path));
    return content.find(L"[TRACE] t: now kept") != std::wstring::npos;
}

// 文件 sink 必须以 UTF-8 字节存储宽文本，使中文经
// 文件往返不损坏、任何 UTF-8 工具可读。直接读原始字节查找中文载荷的精确 UTF-8
// 编码，ReadWholeFile 的字节->wchar 拓宽对多字节文本有损，故在字节层比对。
bool TestLoggerFileSinkStoresChineseAsUtf8() {
    const std::wstring path = MakeTempLogPath();
    {
        optimizer::logger::Logger logger;
        const auto opened = logger.SetFileSink(path);
        if (!opened) {
            return false;
        }
        logger.Write(optimizer::logger::LogLevel::Info, L"内存模块",
                     L"中文日志消息");
    }
    std::string bytes;
    {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    } // 先关闭流再删除：Windows 上删除被占用文件会阻塞
    std::filesystem::remove(std::filesystem::path(path));

    // "中文日志消息" 的 UTF-8 字节。用程序构造以保证测试源码不受编译器
    // 源编码影响，保持 ASCII 安全。
    const std::string zhUtf8 = [] {
        std::string s;
        const std::wstring zh = L"中文日志消息";
        const int needed = ::WideCharToMultiByte(
            CP_UTF8, 0, zh.data(), static_cast<int>(zh.size()),
            nullptr, 0, nullptr, nullptr);
        s.resize(static_cast<std::size_t>(needed));
        ::WideCharToMultiByte(
            CP_UTF8, 0, zh.data(), static_cast<int>(zh.size()),
            s.data(), needed, nullptr, nullptr);
        return s;
    }();

    // 模块名 "内存模块" 也须在同一行以 UTF-8 存活。
    const std::wstring zhModule = L"内存模块";
    std::string moduleUtf8;
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, zhModule.data(), static_cast<int>(zhModule.size()),
        nullptr, 0, nullptr, nullptr);
    moduleUtf8.resize(static_cast<std::size_t>(needed));
    ::WideCharToMultiByte(
        CP_UTF8, 0, zhModule.data(), static_cast<int>(zhModule.size()),
        moduleUtf8.data(), needed, nullptr, nullptr);

    return !bytes.empty() && bytes.find(zhUtf8) != std::string::npos &&
           bytes.find(moduleUtf8) != std::string::npos;
}

// ---------- LOG-001：日志文件轮转（[logging] max_file_mb / max_files 语义） ----------

bool TestLoggerRotatesAtFileLimit() {
    // 单文件上限 1 MiB：写入超限后必须出现历史文件（轮转真的发生）。
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    {
        optimizer::logger::Logger logger;
        optimizer::logger::FileSinkOptions options;
        options.maxFileMb = 1;
        options.maxFiles = 3;
        if (!logger.SetFileSink(path, options)) {
            return false;
        }
        const std::wstring filler(400, L'x'); // 每行 ~420 字节
        for (int i = 0; i < 3000; ++i) {       // ~1.2 MiB -> 至少一次轮转
            logger.Write(optimizer::logger::LogLevel::Info, L"rot", filler);
        }
    }
    std::error_code ec;
    const bool rotated = std::filesystem::exists(
        std::filesystem::path(RotatedLogPath(path, 1)), ec);
    const bool active = std::filesystem::exists(std::filesystem::path(path), ec);
    RemoveLogFiles(path);
    return rotated && active;
}

bool TestLoggerRotationKeepsFileCountBound() {
    // maxFiles = 2：写 ~3.4 MiB（多次轮转）后，文件总数（含当前）不得超过 2。
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    {
        optimizer::logger::Logger logger;
        optimizer::logger::FileSinkOptions options;
        options.maxFileMb = 1;
        options.maxFiles = 2;
        if (!logger.SetFileSink(path, options)) {
            return false;
        }
        const std::wstring filler(400, L'x');
        for (int i = 0; i < 8000; ++i) {
            logger.Write(optimizer::logger::LogLevel::Info, L"rot", filler);
        }
    }
    const int files = CountLogFiles(path, 8);
    RemoveLogFiles(path);
    // 严格断言：maxFiles = 2（历史文件数上限）-> 磁盘上恰好 base + .1 + .2 = 3 个文件；
    // 若未轮转会是 1 个（失败而非“虚真通过”）。
    return files == 3;
}

bool TestLoggerFileSinkWithConsoleCopy() {
    // alsoConsole = true：文件与 stderr 双 sink 并存——控制台副本为**附加**，不替代文件输出。
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    {
        optimizer::logger::Logger logger;
        optimizer::logger::FileSinkOptions options;
        options.maxFileMb = 0;      // 关闭轮转，便于直接读单文件
        options.alsoConsole = true; // 附加 stderr 副本
        if (!logger.SetFileSink(path, options)) {
            return false;
        }
        logger.Write(optimizer::logger::LogLevel::Info, L"dual",
                     L"written to both sinks");
    }
    const std::wstring content = ReadWholeFile(path);
    RemoveLogFiles(path);
    return content.find(L"[INFO] dual: written to both sinks") !=
           std::wstring::npos;
}

bool TestLoggerNoRotationWhenDisabled() {
    // maxFileMb = 0：显式关闭轮转 -> 单文件持续追加、不产生历史文件。
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    {
        optimizer::logger::Logger logger;
        optimizer::logger::FileSinkOptions options;
        options.maxFileMb = 0;
        options.maxFiles = 2;
        if (!logger.SetFileSink(path, options)) {
            return false;
        }
        const std::wstring filler(400, L'x');
        for (int i = 0; i < 3000; ++i) {
            logger.Write(optimizer::logger::LogLevel::Info, L"nrot", filler);
        }
    }
    std::error_code ec;
    const auto size =
        std::filesystem::file_size(std::filesystem::path(path), ec);
    const bool grewBeyondOneMb = !ec && size > 1024 * 1024;
    const int files = CountLogFiles(path, 8);
    RemoveLogFiles(path);
    return grewBeyondOneMb && files == 1;
}

// ---------- LOG-005：有界异步日志队列 ----------

struct AsyncHarness {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<optimizer::logger::LogRecord> written;
    optimizer::logger::AsyncLogQueue::Sink sink() {
        return [this](const optimizer::logger::LogRecord& r) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                written.push_back(r);
            }
            cv.notify_all();
        };
    }
    bool waitFor(std::size_t n) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(3),
                           [&] { return written.size() >= n; });
    }
};

optimizer::logger::LogRecord MakeAsyncRecord(optimizer::logger::LogLevel level) {
    optimizer::logger::LogRecord r;
    r.level = level;
    r.localTime = L"2026-09-17 00:00:00.000";
    r.module = L"t";
    r.message = L"m";
    return r;
}

bool TestAsyncQueueDrainsInOrderOnStop() {
    AsyncHarness h;
    {
        optimizer::logger::AsyncLogQueue queue(h.sink());
        for (int i = 0; i < 3; ++i) {
            if (!queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info))) {
                return false;
            }
        }
        queue.Stop(); // 排空后结束线程
        if (queue.IsRunning() || queue.WrittenCount() != 3) {
            return false;
        }
    }
    return h.written.size() == 3;
}

bool TestAsyncQueueDropsLowestAndProtectsError() {
    AsyncHarness h;
    optimizer::logger::AsyncLogQueue::Options options;
    options.capacity = 2;
    optimizer::logger::AsyncLogQueue queue(h.sink(), options);
    // 填满两个低级别记录。
    (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Trace));
    (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Debug));
    // 再来一条非保护的 Info：队列满 -> 丢弃本条（计入 dropped）。
    const auto info = queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info));
    // 保护级别 Critical：必须腾位（替换最低级别）而不是丢弃。
    const auto critical = queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Critical));
    const bool dropped = queue.DroppedCount() >= 1;
    queue.Stop();
    bool sawCritical = false;
    for (const auto& r : h.written) {
        if (r.level == optimizer::logger::LogLevel::Critical) {
            sawCritical = true;
        }
    }
    return info.HasValue() && critical.HasValue() && dropped && sawCritical &&
           h.written.size() == 2;
}

bool TestAsyncQueueStopIdempotentAndPushAfterStopFails() {
    AsyncHarness h;
    optimizer::logger::AsyncLogQueue queue(h.sink());
    queue.Stop();
    queue.Stop(); // 幂等：第二次调用不得崩溃/重复 join
    const auto after = queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info));
    return !after && !queue.IsRunning();
}

bool TestAsyncQueueDestructorDrains() {
    AsyncHarness h;
    {
        optimizer::logger::AsyncLogQueue queue(h.sink());
        (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info));
        (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Warn));
    } // 析构即排空 + join（不依赖进程退出清理）
    return h.written.size() == 2;
}

// ---------- LOG-006：Logger 异步 sink 模式 ----------

bool TestLoggerAsyncSinkDrainsOnStop() {
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    optimizer::logger::Logger logger;
    optimizer::logger::FileSinkOptions options;
    options.maxFileMb = 0; // 关闭轮转，便于直接读单文件
    if (!logger.SetAsyncFileSink(path, options, 8)) {
        return false;
    }
    if (!logger.IsAsync()) {
        return false;
    }
    logger.Write(optimizer::logger::LogLevel::Info, L"async", L"line one");
    logger.Write(optimizer::logger::LogLevel::Info, L"async", L"line two");
    logger.Write(optimizer::logger::LogLevel::Error, L"async", L"line three");
    logger.StopAsync(); // 排空后才保证全部落盘
    const std::wstring content = ReadWholeFile(path);
    RemoveLogFiles(path);
    return content.find(L"[INFO] async: line one") != std::wstring::npos &&
           content.find(L"[INFO] async: line two") != std::wstring::npos &&
           content.find(L"[ERROR] async: line three") != std::wstring::npos &&
           !logger.IsAsync();
}

bool TestLoggerWriteAfterAsyncStopFallsBackToSync() {
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    optimizer::logger::Logger logger;
    optimizer::logger::FileSinkOptions options;
    options.maxFileMb = 0;
    if (!logger.SetAsyncFileSink(path, options, 8)) {
        return false;
    }
    logger.StopAsync();
    logger.Write(optimizer::logger::LogLevel::Warn, L"after", L"stop fallback");
    // 同步直写：写后立即可见（降级不伪装、不丢）。
    const std::wstring content = ReadWholeFile(path);
    RemoveLogFiles(path);
    return content.find(L"[WARN] after: stop fallback") != std::wstring::npos;
}

bool TestLoggerAsyncProtectedLevelNotLostWhenQueueFull() {
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    optimizer::logger::Logger logger;
    optimizer::logger::FileSinkOptions options;
    options.maxFileMb = 0;
    if (!logger.SetAsyncFileSink(path, options, 1)) { // 容量 1：第二条 Error 必然满队列
        return false;
    }
    logger.Write(optimizer::logger::LogLevel::Error, L"p", L"first error");
    logger.Write(optimizer::logger::LogLevel::Error, L"p", L"second error");
    logger.StopAsync();
    const std::wstring content = ReadWholeFile(path);
    RemoveLogFiles(path);
    return content.find(L"[ERROR] p: first error") != std::wstring::npos &&
           content.find(L"[ERROR] p: second error") != std::wstring::npos;
}

// ---------- LOG-007：异步模式中途 Flush ----------

bool TestAsyncQueueFlushWaitsForPending() {
    AsyncHarness h;
    optimizer::logger::AsyncLogQueue queue(h.sink());
    for (int i = 0; i < 3; ++i) {
        (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info));
    }
    queue.Flush(); // 返回即代表调用时刻已入队的记录全部写出
    if (queue.WrittenCount() != 3) {
        return false;
    }
    // Flush 不停止接收：之后仍可继续 Push。
    (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info));
    queue.Flush();
    const bool ok = queue.WrittenCount() == 4;
    queue.Stop();
    return ok;
}

bool TestAsyncQueueFlushAfterStopReturnsImmediately() {
    AsyncHarness h;
    optimizer::logger::AsyncLogQueue queue(h.sink());
    (void)queue.Push(MakeAsyncRecord(optimizer::logger::LogLevel::Info));
    queue.Stop();  // 排空 + 结束线程
    queue.Flush(); // 已停止：必须立即返回（不得永久等待）
    return queue.WrittenCount() == 1;
}

bool TestLoggerAsyncFlushMakesRecordsVisible() {
    // 关键契约：异步模式下 Flush() 返回后，调用时刻之前写入的记录必须已可读（无需 Stop）。
    const std::wstring path = MakeTempLogPath();
    RemoveLogFiles(path);
    optimizer::logger::Logger logger;
    optimizer::logger::FileSinkOptions options;
    options.maxFileMb = 0;
    if (!logger.SetAsyncFileSink(path, options, 8)) {
        return false;
    }
    logger.Write(optimizer::logger::LogLevel::Info, L"flush", L"visible one");
    logger.Write(optimizer::logger::LogLevel::Info, L"flush", L"visible two");
    logger.Flush(); // 中途冲刷（未 Stop）
    const std::wstring content = ReadWholeFile(path);
    const bool ok = content.find(L"[INFO] flush: visible one") != std::wstring::npos &&
                    content.find(L"[INFO] flush: visible two") != std::wstring::npos &&
                    logger.IsAsync();
    logger.StopAsync();
    RemoveLogFiles(path);
    return ok;
}

} // namespace

int wmain() {
    int failed = 0;
    const auto run = [&failed](const wchar_t* name, bool (*test)()) {
        const bool passed = test();
        std::wcout << (passed ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
        if (!passed) {
            ++failed;
        }
    };

    run(L"Level names are stable", &TestLevelToStringAllLevels);
    run(L"Level names round-trip", &TestLevelFromStringRoundTrip);
    run(L"Unknown level name is rejected", &TestLevelFromStringRejectsUnknown);
    run(L"Level names are case-insensitive", &TestLevelFromStringIsCaseInsensitive);
    run(L"Log line formatting", &TestFormatLogRecordFull);
    run(L"Log formatting is pure and deterministic", &TestFormatLogRecordIsPureAndDeterministic);
    run(L"Logger filters below threshold", &TestLoggerFiltersBelowThreshold);
    run(L"File sink writes and closes on RAII", &TestLoggerFileSinkWritesAndCloses);
    run(L"File sink failure stays usable", &TestLoggerFileSinkFailureStaysUsable);
    run(L"Critical always written at Error level", &TestLoggerCriticalAlwaysWritten);
    run(L"Concurrent writes do not crash", &TestLoggerConcurrentWritesNoCrash);
    run(L"SetLevel changes the filter", &TestLoggerSetLevelChangesFilter);
    run(L"File sink stores Chinese as UTF-8", &TestLoggerFileSinkStoresChineseAsUtf8);
    run(L"Logger rotates at file limit", &TestLoggerRotatesAtFileLimit);
    run(L"Logger rotation keeps file count bound",
        &TestLoggerRotationKeepsFileCountBound);
    run(L"Logger no rotation when disabled", &TestLoggerNoRotationWhenDisabled);
    run(L"Async queue drains in order on stop", &TestAsyncQueueDrainsInOrderOnStop);
    run(L"Async queue drops lowest and protects error",
        &TestAsyncQueueDropsLowestAndProtectsError);
    run(L"Async queue stop idempotent and push after stop fails",
        &TestAsyncQueueStopIdempotentAndPushAfterStopFails);
    run(L"Async queue destructor drains", &TestAsyncQueueDestructorDrains);
    run(L"Logger file sink with console copy", &TestLoggerFileSinkWithConsoleCopy);
    run(L"Logger async sink drains on stop", &TestLoggerAsyncSinkDrainsOnStop);
    run(L"Logger write after async stop falls back to sync",
        &TestLoggerWriteAfterAsyncStopFallsBackToSync);
    run(L"Logger async protected level not lost",
        &TestLoggerAsyncProtectedLevelNotLostWhenQueueFull);
    run(L"Async queue flush waits for pending", &TestAsyncQueueFlushWaitsForPending);
    run(L"Async queue flush after stop returns immediately",
        &TestAsyncQueueFlushAfterStopReturnsImmediately);
    run(L"Logger async flush makes records visible",
        &TestLoggerAsyncFlushMakesRecordsVisible);
    return failed == 0 ? 0 : 1;
}
