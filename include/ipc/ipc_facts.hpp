#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::ipc {

// FactsSnapshot 载荷结构化契约 v1（IPC-002，Agent 应用层首切片）。
//
// 帧协议（ipc_protocol.hpp）只保证"一帧完整、帧头严格校验"；IPC-001 中
// FactsSnapshot 载荷是不透明字节（服务端只回"N bytes"）。IPC-002 起把载荷
// 定义为两端约定的结构化 UTF-8 文本：
//   - 首行信封：CPOPFACTS/1（与帧协议版本相互独立，载荷内部另行版本化）；
//   - 后续每行一条 key=value（行尾只允许 LF；CR 出现即整体拒绝）；
//   - 键：ASCII 字母/数字/_/-，1..kMaxFactsKeyBytes 字节；
//   - 值：0..kMaxFactsValueBytes 字节 UTF-8 文本（允许空格与多字节字符，
//     禁止控制字节——保证回显与日志不被注入）；
//   - 至多 kMaxFactsEntries 条；重复键整体拒绝（歧义即拒绝，不取"最后者胜"）；
//   - 不做宽松转换：任何一项违反立即整体拒绝（Validation），与帧级严格校验
//     同一哲学。
//
// 本层只负责"信封与语法"，不解释键的语义：服务端接受哪些键、键值如何参与
// 决策（请求白名单、会话校验）仍属后续 Agent 切片。Serialize/Parse 互逆。

// 载荷信封（首行）。
inline constexpr std::string_view kFactsEnvelopeV1 = "CPOPFACTS/1";

// 单载荷最多事实条数。
inline constexpr std::size_t kMaxFactsEntries = 64;
// 键最大字节数（ASCII 可见子集）。
inline constexpr std::size_t kMaxFactsKeyBytes = 64;
// 值最大字节数（UTF-8 文本）。
inline constexpr std::size_t kMaxFactsValueBytes = 128;
// 服务端回执摘要上限（UTF-8 字节）。限制回显体积，保证应答载荷远低于帧上限。
inline constexpr std::size_t kFactsSummaryMaxBytes = 480;

// 单条事实：键 + UTF-8 文本值。
struct IpcFact {
    std::string key;
    std::string value;
};

// 严格解析 FactsSnapshot 载荷（UTF-8 文本）。失败返回 Validation（不宽松接受）。
[[nodiscard]] common::Result<std::vector<IpcFact>> ParseFactsV1(
    std::span<const std::byte> payload);

// 编码为 UTF-8 文本字节（与 ParseFactsV1 互逆）。输入违反契约返回 Validation。
[[nodiscard]] common::Result<std::vector<std::byte>> SerializeFactsV1(
    std::span<const IpcFact> facts);

// 紧凑回执摘要文本（UTF-8）："facts ok (N): k=v, ..."。超限时在整条事实边界
// 截断并追加 " …"（不切分多字节字符）。仅用于应答/日志回显；输入须已通过
// Parse/Serialize 校验（无控制字节）。
[[nodiscard]] std::string FormatFactsSummary(std::span<const IpcFact> facts);

} // namespace optimizer::ipc
