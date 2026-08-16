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
    } // logger 在此析构：句柄关闭（RAII），内容已刷出
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
    // 指向已存在目录的路径打开为文件必然失败（spdlog 会自动创建缺失父目录，
    // 所以不存在的子目录不再失败）：logger 仍可用且不抛异常。
    const std::wstring badPath = MakeTempLogPath() + L"_dir";
    std::filesystem::create_directory(std::filesystem::path(badPath));
    const auto opened = logger.SetFileSink(badPath);
    std::filesystem::remove(std::filesystem::path(badPath));
    if (opened.HasValue()) {
        return false;
    }
    // logger 仍须接受写入而不抛（降级到 Debug）。
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

// 中文支持（工程手册 8.1）：文件 sink 必须以 UTF-8 字节存储宽文本，使中文经
// 文件往返不损坏、任何 UTF-8 工具可读。直接读原始字节查找中文载荷的精确 UTF-8
// 编码（ReadWholeFile 的字节->wchar 拓宽对多字节文本有损，故在字节层比对）。
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
    // 源编码影响（保持 ASCII 安全）。
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
    run(L"Log line formatting", &TestFormatLogRecordFull);
    run(L"Log formatting is pure and deterministic", &TestFormatLogRecordIsPureAndDeterministic);
    run(L"Logger filters below threshold", &TestLoggerFiltersBelowThreshold);
    run(L"File sink writes and closes on RAII", &TestLoggerFileSinkWritesAndCloses);
    run(L"File sink failure stays usable", &TestLoggerFileSinkFailureStaysUsable);
    run(L"Critical always written at Error level", &TestLoggerCriticalAlwaysWritten);
    run(L"Concurrent writes do not crash", &TestLoggerConcurrentWritesNoCrash);
    run(L"SetLevel changes the filter", &TestLoggerSetLevelChangesFilter);
    run(L"File sink stores Chinese as UTF-8", &TestLoggerFileSinkStoresChineseAsUtf8);
    return failed == 0 ? 0 : 1;
}
