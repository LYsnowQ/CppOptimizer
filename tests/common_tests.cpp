#include "common/error.hpp"
#include "common/unique_resource.hpp"

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
    return failed == 0 ? 0 : 1;
}
