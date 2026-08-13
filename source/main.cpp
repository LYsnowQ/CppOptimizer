#include "common/error.hpp"
#include "logger/logger.hpp"
#include "memory/memory_tuner.hpp"
#include "metrics/memory_metrics.hpp"
#include "platform/native_api.hpp"

#include <chrono>
#include <cwchar>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int RunStatus() {
    const auto result = optimizer::memory::QueryMemoryStatus();
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wcerr << L"  memory snapshot : failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& status = result.Value();
    const auto now = std::chrono::steady_clock::now();
    const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - status.sampledAt)
                           .count();
    constexpr auto kStatusFreshnessWindow = std::chrono::seconds(30);

    std::wcout << L"Memory snapshot (read-only, single query)\n";
    std::wcout << L"  total      : "
               << optimizer::memory::FormatBytes(status.totalPhysicalBytes) << L" ("
               << status.totalPhysicalBytes << L" bytes)\n";
    std::wcout << L"  available  : "
               << optimizer::memory::FormatBytes(status.availablePhysicalBytes) << L" ("
               << status.availablePhysicalBytes << L" bytes)\n";
    std::wcout << L"  used       : "
               << optimizer::memory::FormatBytes(status.usedPhysicalBytes) << L" ("
               << status.usedPhysicalBytes << L" bytes)\n";
    std::wcout << L"  load       : " << status.memoryLoadPercent << L"%\n";
    std::wcout << L"  age        : " << ageMs << L" ms (fresh: "
               << (optimizer::memory::IsSnapshotFresh(
                       status.sampledAt, now, kStatusFreshnessWindow)
                       ? L"yes"
                       : L"no")
               << L")\n";
    return 0;
}

int RunObserve(std::wstring_view secondsText, std::wstring_view thresholdText) {
    // Foreground, bounded, user-invoked observation: no background thread, no
    // periodic task, no system writes. This is metrics sampling, not polling a
    // cleaner; the red zone rules for automatic memory cleaning do not apply.
    constexpr std::uint32_t kMaxSeconds = 60;
    constexpr std::uint32_t kDefaultLowLoadThreshold = 50;
    const std::wstring secondsTextCopy(secondsText);
    const std::uint32_t seconds = static_cast<std::uint32_t>(
        std::wcstoul(secondsTextCopy.c_str(), nullptr, 10));
    if (seconds == 0 || seconds > kMaxSeconds) {
        std::wcerr << L"  --observe seconds must be in 1.." << kMaxSeconds << L"\n";
        return 2;
    }

    // Optional low-load threshold (0..100, default 50). Parsing lives in the
    // command layer only; the pure function re-validates the domain.
    std::uint32_t threshold = kDefaultLowLoadThreshold;
    if (!thresholdText.empty()) {
        const std::wstring thresholdTextCopy(thresholdText);
        const std::uint32_t parsed = static_cast<std::uint32_t>(
            std::wcstoul(thresholdTextCopy.c_str(), nullptr, 10));
        if (parsed > 100) {
            std::wcerr << L"  --observe threshold must be in 0..100\n";
            return 2;
        }
        threshold = parsed;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::vector<optimizer::metrics::MemorySample> samples;
    samples.reserve(seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = optimizer::memory::QueryMemoryStatus();
        if (!status) {
            const auto& error = status.ErrorValue();
            std::wcerr << L"  memory sample failed ["
                       << optimizer::common::ToString(error.domain) << L":"
                       << error.code << L"] " << error.message << L"\n";
            return 2;
        }
        samples.push_back(optimizer::metrics::MemorySample{
            status.Value().memoryLoadPercent,
            status.Value().availablePhysicalBytes});
        if (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    auto report = optimizer::metrics::AggregateMemoryWindow(samples);
    if (!report) {
        const auto& error = report.ErrorValue();
        std::wcerr << L"  window aggregation failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    auto share = optimizer::metrics::ShareOfLoadBelow(samples, threshold);
    if (!share) {
        const auto& error = share.ErrorValue();
        std::wcerr << L"  low-load share failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& r = report.Value();
    std::wcout << L"Memory observation window (read-only, foreground, " << seconds
               << L" s)\n";
    std::wcout << L"  samples      : " << r.sampleCount << L"\n";
    std::wcout << L"  load %       : min " << r.minLoadPercent << L" / avg "
               << r.avgLoadPercent << L" / max " << r.maxLoadPercent << L"\n";
    std::wcout << L"  available    : min "
               << optimizer::memory::FormatBytes(r.minAvailableBytes) << L" / max "
               << optimizer::memory::FormatBytes(r.maxAvailableBytes) << L"\n";
    std::wcout << L"  load < " << threshold << L"%   : " << share.Value()
               << L"% of samples\n";
    return 0;
}

int RunLogCommand(int argc, wchar_t* argv[]) {
    // --log <module> <message...>: one synchronous Info record to stderr.
    // Read-only, foreground, no background thread; message text is never
    // treated as a command. At least one message token is required.
    if (argc < 4) {
        std::wcerr << L"  --log requires a module and a message\n";
        return 2;
    }
    std::wstring message;
    for (int i = 3; i < argc; ++i) {
        if (i > 3) {
            message.push_back(L' ');
        }
        message.append(argv[i]);
    }
    optimizer::logger::Logger logger;
    logger.SetStderrSink();
    logger.Write(optimizer::logger::LogLevel::Info, argv[2], message);
    return 0;
}

int RunDiagnostics() {
    std::wcout << L"CppOptimizer diagnostics\n";
    std::wcout << L"  process mode : user mode (Ring 3)\n";
#if defined(_M_X64)
    std::wcout << L"  architecture : x64\n";
#elif defined(_M_IX86)
    std::wcout << L"  architecture : x86 (not a release target)\n";
#else
    std::wcout << L"  architecture : unsupported/unknown\n";
#endif
    std::wcout << L"  safe default : observe only\n";

    auto result = optimizer::platform::NativeApi::Instance().Probe();
    if (!result) {
        const auto& error = result.ErrorValue();
        std::wcerr << L"  native probe : failed ["
                   << optimizer::common::ToString(error.domain) << L":"
                   << error.code << L"] " << error.message << L"\n";
        return 2;
    }

    const auto& capabilities = result.Value();
    std::wcout << L"  ntdll loaded : " << (capabilities.ntdllLoaded ? L"yes" : L"no") << L"\n";
    std::wcout << L"  NtQuerySystemInformation : "
               << (capabilities.querySystemInformation ? L"available" : L"unavailable") << L"\n";
    std::wcout << L"  NtSetSystemInformation   : "
               << (capabilities.setSystemInformation ? L"available (write remains disabled)" : L"unavailable")
               << L"\n";
    std::wcout << L"  RtlNtStatusToDosError    : "
               << (capabilities.ntStatusConversion ? L"available" : L"unavailable") << L"\n";
    return 0;
}

void PrintUsage() {
    std::wcout
        << L"CppOptimizer (engineering baseline)\n\n"
        << L"Usage:\n"
        << L"  CppOptimizer.exe --diagnose   Show safe, read-only platform diagnostics\n"
        << L"  CppOptimizer.exe --status     Show one read-only memory snapshot\n"
        << L"  CppOptimizer.exe --observe <s> [threshold] Sample each second for 1..60 s;\n"
        << L"                             optional low-load threshold 0..100 (default 50)\n"
        << L"  CppOptimizer.exe --log <module> <message...> Write one Info log line to stderr\n"
        << L"                             (read-only, foreground, bounded)\n"
        << L"  CppOptimizer.exe --help       Show this message\n\n"
        << L"No optimization action is enabled in this build entry point.\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--diagnose") {
            return RunDiagnostics();
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--status") {
            return RunStatus();
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--observe") {
            return RunObserve(argv[2], L"");
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--observe") {
            return RunObserve(argv[2], argv[3]);
        }
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--log") {
            return RunLogCommand(argc, argv);
        }
        PrintUsage();
        return argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help") ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal C++ exception: " << exception.what() << '\n';
        return 100;
    } catch (...) {
        std::cerr << "Fatal unknown exception\n";
        return 101;
    }
}
