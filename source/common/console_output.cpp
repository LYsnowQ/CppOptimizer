#include "common/console_output.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <utility>

namespace optimizer::common {

Result<std::string> WideToUtf8(std::wstring_view text) noexcept {
    if (text.empty()) {
        return Result<std::string>::Success(std::string());
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed == 0) {
        return Result<std::string>::Failure(
            Error::FromWin32(::GetLastError(), "WideCharToMultiByte"));
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          utf8.data(), needed, nullptr, nullptr);
    return Result<std::string>::Success(std::move(utf8));
}

Result<std::wstring> Utf8ToWide(std::string_view text) noexcept {
    if (text.empty()) {
        return Result<std::wstring>::Success(std::wstring());
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (needed == 0) {
        return Result<std::wstring>::Failure(Error::FromWin32(
            ::GetLastError(), "MultiByteToWideChar(CP_UTF8)"));
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(), needed);
    return Result<std::wstring>::Success(std::move(wide));
}

namespace {

// 双路径写句柄：直连真实控制台用 WriteConsoleW 宽字符直写（绕过代码页 936）；
// 重定向到文件/管道用 UTF-8 字节。句柄无效或写入失败静默（不阻断命令主流程）。
void WriteConsoleHandle(HANDLE handle, std::wstring_view text) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    if (::GetFileType(handle) == FILE_TYPE_CHAR) {
        DWORD written = 0;
        ::WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()),
                        &written, nullptr);
        return;
    }
    auto utf8 = WideToUtf8(text);
    if (!utf8) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(handle, utf8.Value().data(),
                static_cast<DWORD>(utf8.Value().size()), &written, nullptr);
}

} // namespace

void WriteConsole(std::wstring_view text) noexcept {
    WriteConsoleHandle(::GetStdHandle(STD_OUTPUT_HANDLE), text);
}

void WriteConsoleLine(std::wstring_view text) noexcept {
    WriteConsole(text);
    WriteConsole(L"\n");
}

void WriteConsoleError(std::wstring_view text) noexcept {
    WriteConsoleHandle(::GetStdHandle(STD_ERROR_HANDLE), text);
}

void WriteConsoleErrorLine(std::wstring_view text) noexcept {
    WriteConsoleError(text);
    WriteConsoleError(L"\n");
}

} // namespace optimizer::common
