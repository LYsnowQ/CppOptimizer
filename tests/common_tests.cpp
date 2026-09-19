#include "common/error.hpp"
#include "common/unique_resource.hpp"
#include "common/console_output.hpp"

#include <iostream>
#include <utility>

namespace {

bool TestResultValue() {
    auto result = optimizer::common::Result<int>::Success(42);
    return result.HasValue() && result.Value() == 42;
}

bool TestResultError() {
    auto result = optimizer::common::Result<int>::Failure(
        optimizer::common::Error::Validation("test", L"expected failure"));
    return !result.HasValue() &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Validation;
}

bool TestUniqueHandleMove() {
    HANDLE rawEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (rawEvent == nullptr) {
        return false;
    }

    optimizer::common::UniqueHandle first(rawEvent);
    optimizer::common::UniqueHandle second(std::move(first));
    return !first.IsValid() && second.IsValid() && second.Get() == rawEvent;
}

bool TestWideToUtf8() {
    // 中文往返：宽字符 -> UTF-8 字节 -> 回读一致。
    auto utf8 = optimizer::common::WideToUtf8(L"中文标题测试");
    if (!utf8.HasValue()) {
        return false;
    }
    const std::string expected = "中文标题测试";
    return utf8.Value() == expected;
}

bool TestWideToUtf8Empty() {
    auto utf8 = optimizer::common::WideToUtf8(L"");
    return utf8.HasValue() && utf8.Value().empty();
}

bool TestUtf8ToWideRoundTrip() {
    // 与 WideToUtf8 互为逆：中文往返必须逐字符一致；空串返回空串。
    const std::wstring original = L"中文 UTF-8 往返 audit=1";
    const auto utf8 = optimizer::common::WideToUtf8(original);
    if (!utf8) {
        return false;
    }
    const auto back = optimizer::common::Utf8ToWide(utf8.Value());
    const auto empty = optimizer::common::Utf8ToWide("");
    return back && back.Value() == original && empty &&
           empty.Value().empty();
}

bool TestUtf8ToWideRejectsInvalidBytes() {
    // 孤立续字节 0x80：非法 UTF-8，必须如实失败（不宽松替换成 U+FFFD）。
    const std::string invalid = "\x80\x80";
    const auto result = optimizer::common::Utf8ToWide(invalid);
    return !result &&
           result.ErrorValue().domain == optimizer::common::ErrorDomain::Win32;
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

    run(L"Result stores values", &TestResultValue);
    run(L"Result stores errors", &TestResultError);
    run(L"UniqueHandle move transfers ownership", &TestUniqueHandleMove);
    run(L"WideToUtf8 keeps Chinese round-trip", &TestWideToUtf8);
    run(L"WideToUtf8 empty", &TestWideToUtf8Empty);
    run(L"Utf8ToWide keeps Chinese round-trip", &TestUtf8ToWideRoundTrip);
    run(L"Utf8ToWide rejects invalid bytes", &TestUtf8ToWideRejectsInvalidBytes);
    return failed == 0 ? 0 : 1;
}
