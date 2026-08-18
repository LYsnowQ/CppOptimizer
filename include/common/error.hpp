#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace optimizer::common {

enum class ErrorDomain {
    Win32,
    HResult,
    Pdh,
    NtStatus,
    Validation,
    Unsupported,
    Internal
};

// 统一错误模型：domain 标识来源域，code 为原始错误码，
// operation 为出错的操作名，message 为可显示信息。
struct Error {
    ErrorDomain domain = ErrorDomain::Internal;
    std::uint64_t code = 0;
    std::string operation;
    std::wstring message;

    static Error FromWin32(std::uint32_t code, std::string operation);
    static Error FromHResult(std::int32_t code, std::string operation);
    static Error FromPdh(std::int32_t code, std::string operation);
    static Error FromNtStatus(std::int32_t code, std::string operation);
    static Error Unsupported(std::string operation, std::wstring message);
    static Error Validation(std::string operation, std::wstring message);
};

std::wstring FormatErrorMessage(ErrorDomain domain, std::uint64_t code);
const wchar_t* ToString(ErrorDomain domain) noexcept;

// 失败不伪装成成功：任何可能失败的接口都应返回 Result，调用方必须检查
// HasValue()/operator bool 后再访问 Value()/ErrorValue()。
template <typename T>
class Result {
public:
    static Result Success(T value) {
        return Result(std::move(value));
    }

    static Result Failure(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool HasValue() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    explicit operator bool() const noexcept {
        return HasValue();
    }

    T& Value() & {
        return std::get<T>(storage_);
    }

    const T& Value() const& {
        return std::get<T>(storage_);
    }

    T&& Value() && {
        return std::get<T>(std::move(storage_));
    }

    Error& ErrorValue() & {
        return std::get<Error>(storage_);
    }

    const Error& ErrorValue() const& {
        return std::get<Error>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

// Result<void> 特化：只有成功/失败两种状态，无值负载。
template <>
class Result<void> {
public:
    static Result Success() {
        return Result(std::monostate{});
    }

    static Result Failure(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool HasValue() const noexcept {
        return std::holds_alternative<std::monostate>(storage_);
    }

    explicit operator bool() const noexcept {
        return HasValue();
    }

    Error& ErrorValue() & {
        return std::get<Error>(storage_);
    }

    const Error& ErrorValue() const& {
        return std::get<Error>(storage_);
    }

private:
    explicit Result(std::monostate value) : storage_(value) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<std::monostate, Error> storage_;
};

} // namespace optimizer::common
