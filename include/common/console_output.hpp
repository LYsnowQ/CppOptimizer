#pragma once

#include "common/error.hpp"

#include <string>
#include <string_view>

namespace optimizer::common {

// 宽字符串转 UTF-8 字节串（纯函数，可单测）。
// 非法 UTF-16 代理项返回 Validation 错误。
[[nodiscard]] Result<std::string> WideToUtf8(std::wstring_view text) noexcept;

// 宽文本控制台输出（双路径）：
// - 直连真实控制台（FILE_TYPE_CHAR）：WriteConsoleW 宽字符直写，绕过代码页 936；
// - 重定向至文件/管道：UTF-8 字节写出，保持可移植性。
// 替代 std::wcout：默认 C locale 下 wcout 无法转换非 ASCII 宽字符，
// 遇中文进入 failbit 后丢弃后续输出。
// 不抛异常；失败静默（控制台输出失败不阻断命令主流程）。
void WriteConsole(std::wstring_view text) noexcept;

// WriteConsole + 换行。
void WriteConsoleLine(std::wstring_view text) noexcept;

} // namespace optimizer::common
