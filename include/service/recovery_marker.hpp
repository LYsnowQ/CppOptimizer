#pragma once

#include "common/error.hpp"

#include <filesystem>
#include <string_view>

namespace optimizer::service {

// 上次异常退出恢复标记（IPC-018，docs/23 §6“上次异常退出且恢复未确认”触发项）。
//
// 机制：会话开始时写标记、**正常结束**时清除；若进程异常退出（崩溃/被杀/失败返回），标记
// 留存。下次启动发现标记仍存在 = 上次会话未正常结束 -> 宿主进入 Safe Mode（离散异常 latch，
// 只允许 R0/恢复动作）直到显式确认（CLI `--confirm-recovery` 清除标记）。
//
// 存储：%LOCALAPPDATA%\CppOptimizer\recovery-state.json（与 ipc_credentials token 同目录）。
// 内容为最小结构化文本（首行信封 + 状态行），避免把目录内其它文件误认为恢复标记。

// 标记文件内容信封（首行）。次行为 "unclean"（标记语义）。
inline constexpr std::string_view kRecoveryMarkerEnvelope =
    "CppOptimizerRecovery/1";

// 默认标记路径：%LOCALAPPDATA%\CppOptimizer\recovery-state.json；
// 环境变量缺失时回退系统 Temp 目录。
[[nodiscard]] std::filesystem::path DefaultRecoveryMarkerPath() noexcept;

// 写入标记（覆盖；父目录自建）。失败返回 Failure（如实上报，不伪装）。
[[nodiscard]] common::Result<void> WriteRecoveryMarker(
    const std::filesystem::path& path) noexcept;

// 清除标记（正常结束/显式确认）。文件不存在视为成功（幂等）。
[[nodiscard]] common::Result<void> ClearRecoveryMarker(
    const std::filesystem::path& path) noexcept;

// 标记是否存在：文件存在且内容首行信封匹配才算设置（异常退出未确认）；
// 不存在/内容不符返回 false；读取失败（IO 错误）返回 Failure 由调用方定夺。
[[nodiscard]] common::Result<bool> IsRecoveryMarkerSet(
    const std::filesystem::path& path) noexcept;

} // namespace optimizer::service
