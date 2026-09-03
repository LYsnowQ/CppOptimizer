#pragma once

#include "common/error.hpp"

#include <filesystem>
#include <string>

namespace optimizer::ipc {

// IPC-007：agent_token 真实供给（替换 IPC-005 的 demo 明文传参）。
//
// 设计（信任边界：Agent 与 Service 同机互信，本实现供给“按用户私有”的共享秘密）：
//   - token = 32 位 [A-Za-z0-9] 随机串（一次生成），写入用户私有文件
//     %LOCALAPPDATA%\CppOptimizer\ipc_agent_token；
//   - 文件显式 DACL：仅 SYSTEM 与当前用户（保护 DACL 不继承、不放行 Everyone/
//     BUILTIN\Users 等扩大面），供未来 SYSTEM 服务（ServiceHost）与同用户 Agent 读取；
//   - 服务端 Options.expectedToken 与客户端 agent_token 事实均从该存储加载，
//     校验仍走 IPC-005/006 既有路径（本模块只负责“供给”不重复“校验”）；
//   - 不提供“打印 token”接口（防泄漏）；Provision 幂等（已存在即失败），
//     轮换用 force（重写为新随机串）。
//
// 真实部署注意（非本实现范围）：多用户/跨会话场景下 SYSTEM 服务需按会话用户
// 定位其私有文件（模拟/按 SID 目录），以及失败计数与安全模式联动。

// 默认私有存储路径（LOCALAPPDATA\CppOptimizer\ipc_agent_token）；
// LOCALAPPDATA 不可用时返回空串（调用方应要求显式路径）。
[[nodiscard]] std::filesystem::path DefaultAgentTokenFilePath() noexcept;

// 生成/轮换私有 token 存储。force=false 且文件已存在 -> Validation（幂等语义）；
// force=true 覆盖为新随机串（轮换）。创建父目录，写后收紧 DACL（SYSTEM + 当前用户）。
[[nodiscard]] common::Result<void> ProvisionAgentTokenFile(
    const std::filesystem::path& path, bool force) noexcept;

// 读取并校验 token（1..64、ASCII 字母/数字/_/-，容忍尾部换行）。
// 失败返回 Validation（含“未供给/内容非法”）；不打印内容。
[[nodiscard]] common::Result<std::wstring> ReadAgentTokenFile(
    const std::filesystem::path& path) noexcept;

// 存储是否已供给（存在且内容合法）。
[[nodiscard]] bool IsAgentTokenProvisioned(
    const std::filesystem::path& path) noexcept;

} // namespace optimizer::ipc
