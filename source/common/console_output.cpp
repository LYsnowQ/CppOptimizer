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

void WriteConsole(std::wstring_view text) noexcept {
    const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    if (::GetFileType(handle) == FILE_TYPE_CHAR) {
        // 直连控制台：宽字符直写，绕过代码页。
        DWORD written = 0;
        ::WriteConsoleW(handle, text.data(),
                        static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    // 重定向：UTF-8 字节写出。
    auto utf8 = WideToUtf8(text);
    if (!utf8) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(handle, utf8.Value().data(),
                static_cast<DWORD>(utf8.Value().size()), &written, nullptr);
}

void WriteConsoleLine(std::wstring_view text) noexcept {
    WriteConsole(text);
    WriteConsole(L"\n");
}

} // namespace optimizer::common
