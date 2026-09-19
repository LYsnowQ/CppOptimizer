#pragma once

#include "common/error.hpp"

#include <string>
#include <string_view>

namespace optimizer::common {

// 宽字符串转 UTF-8 字节串（纯函数，可单测）。
// 非法 UTF-16 代理项返回 Validation 错误。
[[nodiscard]] Result<std::string> WideToUtf8(std::wstring_view text) noexcept;

// UTF-8 字节串转宽字符串（纯函数，可单测）。
// 非法 UTF-8 返回 Win32 错误（MultiByteToWideChar + MB_ERR_INVALID_CHARS）：
// 不做宽松替换，避免把损坏的字节伪装成可读文本。
[[nodiscard]] Result<std::wstring> Utf8ToWide(std::string_view text) noexcept;

// 宽文本控制台输出（双路径）：
// - 直连真实控制台（FILE_TYPE_CHAR）：WriteConsoleW 宽字符直写，绕过代码页 936；
// - 重定向至文件/管道：UTF-8 字节写出，保持可移植性。
// 替代 std::wcout：默认 C locale 下 wcout 无法转换非 ASCII 宽字符，
// 遇中文进入 failbit 后丢弃后续输出。
// 不抛异常；失败静默（控制台输出失败不阻断命令主流程）。
void WriteConsole(std::wstring_view text) noexcept;

// WriteConsole + 换行。
void WriteConsoleLine(std::wstring_view text) noexcept;

// 宽文本 stderr 输出（双路径）：策略与 WriteConsole 相同，但走 STD_ERROR_HANDLE。
// 存在的理由：std::wcerr 在默认 C locale 下遇非 ASCII（如本地化 Win32 错误消息）会进入 failbit
// 并**丢弃之后的所有输出**；用本函数才能保证错误消息不被静默截断（且重定向时仍为 UTF-8 字节）。
void WriteConsoleError(std::wstring_view text) noexcept;

// WriteConsoleError + 换行。
void WriteConsoleErrorLine(std::wstring_view text) noexcept;

} // namespace optimizer::common
