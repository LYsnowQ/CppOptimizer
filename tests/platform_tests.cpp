#include "platform/native_api.hpp"

#include <iostream>

namespace {

using optimizer::platform::ClassifyOsSupport;
using optimizer::platform::OsSupport;
using optimizer::platform::OsVersion;

OsVersion Version(std::uint32_t major, std::uint32_t minor,
                  std::uint32_t build) {
    OsVersion version;
    version.majorVersion = major;
    version.minorVersion = minor;
    version.buildNumber = build;
    return version;
}

bool TestOsSupportAcceptsWindows10And11X64() {
    // Win10 首个发布（1507 = 10.0.10240）及以上、Win11（10.0.22000+）在 x64 上均受支持。
    return ClassifyOsSupport(Version(10, 0, 10240), true) ==
               OsSupport::Supported &&
           ClassifyOsSupport(Version(10, 0, 14393), true) ==
               OsSupport::Supported && // Win10 1607 / Server 2016 同内核
           ClassifyOsSupport(Version(10, 0, 19045), true) ==
               OsSupport::Supported &&
           ClassifyOsSupport(Version(10, 0, 22000), true) ==
               OsSupport::Supported; // Win11
}

bool TestOsSupportRejectsUnsupportedVersions() {
    // build < 10240、旧系统（Win8.1/7）、未识别/未来 major 或 minor 非 0 均不支持。
    return ClassifyOsSupport(Version(10, 0, 10239), true) ==
               OsSupport::UnsupportedVersion &&
           ClassifyOsSupport(Version(6, 3, 9600), true) ==
               OsSupport::UnsupportedVersion && // Win 8.1
           ClassifyOsSupport(Version(6, 1, 7601), true) ==
               OsSupport::UnsupportedVersion && // Win 7
           ClassifyOsSupport(Version(0, 0, 0), true) ==
               OsSupport::UnsupportedVersion && // 查询失败/未知默认
           ClassifyOsSupport(Version(11, 0, 0), true) ==
               OsSupport::UnsupportedVersion && // 未来未识别 major
           ClassifyOsSupport(Version(10, 1, 0), true) ==
               OsSupport::UnsupportedVersion;
}

bool TestOsSupportRejectsNonX64Architecture() {
    // 版本达标但原生架构非 x64：不受支持（产品基线仅 x64）。
    return ClassifyOsSupport(Version(10, 0, 19045), false) ==
           OsSupport::UnsupportedArchitecture;
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
    run(L"os support accepts windows 10/11 x64",
        &TestOsSupportAcceptsWindows10And11X64);
    run(L"os support rejects unsupported versions",
        &TestOsSupportRejectsUnsupportedVersions);
    run(L"os support rejects non-x64 architecture",
        &TestOsSupportRejectsNonX64Architecture);
    return failed == 0 ? 0 : 1;
}
