#include "common/error.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iomanip>
#include <sstream>

namespace optimizer::common {
namespace {

std::wstring FormatSystemMessage(std::uint32_t code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = ::FormatMessageW(
        flags,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr) {
        return {};
    }

    std::wstring message(buffer, length);
    ::LocalFree(buffer);

    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

std::wstring FormatUnknown(ErrorDomain domain, std::uint64_t code) {
    std::wostringstream stream;
    stream << ToString(domain) << L" error 0x"
           << std::hex << std::uppercase << code;
    return stream.str();
}

Error MakeError(ErrorDomain domain, std::uint64_t code, std::string operation) {
    return Error{domain, code, std::move(operation), FormatErrorMessage(domain, code)};
}

} // namespace

Error Error::FromWin32(std::uint32_t code, std::string operation) {
    return MakeError(ErrorDomain::Win32, code, std::move(operation));
}

Error Error::FromHResult(std::int32_t code, std::string operation) {
    return MakeError(
        ErrorDomain::HResult,
        static_cast<std::uint32_t>(code),
        std::move(operation));
}

Error Error::FromPdh(std::int32_t code, std::string operation) {
    return MakeError(
        ErrorDomain::Pdh,
        static_cast<std::uint32_t>(code),
        std::move(operation));
}

Error Error::FromNtStatus(std::int32_t code, std::string operation) {
    return MakeError(
        ErrorDomain::NtStatus,
        static_cast<std::uint32_t>(code),
        std::move(operation));
}

Error Error::Unsupported(std::string operation, std::wstring message) {
    return Error{ErrorDomain::Unsupported, 0, std::move(operation), std::move(message)};
}

Error Error::Validation(std::string operation, std::wstring message) {
    return Error{ErrorDomain::Validation, 0, std::move(operation), std::move(message)};
}

std::wstring FormatErrorMessage(ErrorDomain domain, std::uint64_t code) {
    switch (domain) {
    case ErrorDomain::Win32:
    case ErrorDomain::HResult: {
        const std::wstring message = FormatSystemMessage(static_cast<std::uint32_t>(code));
        return message.empty() ? FormatUnknown(domain, code) : message;
    }
    case ErrorDomain::Pdh:
    case ErrorDomain::NtStatus:
        // 保留原始域与数值码。模块可后续添加专属格式化器；
        // 把它们当 GetLastError 处理是不正确的。
        return FormatUnknown(domain, code);
    case ErrorDomain::Validation:
        return L"Validation error";
    case ErrorDomain::Unsupported:
        return L"Unsupported operation";
    case ErrorDomain::Internal:
    default:
        return FormatUnknown(domain, code);
    }
}

const wchar_t* ToString(ErrorDomain domain) noexcept {
    switch (domain) {
    case ErrorDomain::Win32: return L"Win32";
    case ErrorDomain::HResult: return L"HRESULT";
    case ErrorDomain::Pdh: return L"PDH";
    case ErrorDomain::NtStatus: return L"NTSTATUS";
    case ErrorDomain::Validation: return L"Validation";
    case ErrorDomain::Unsupported: return L"Unsupported";
    case ErrorDomain::Internal: return L"Internal";
    default: return L"Unknown";
    }
}

} // namespace optimizer::common
