#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer::ipc {

// FactsSnapshot 载荷结构化契约 v1(IPC-002,Agent 应用层首切片)。
//
// 帧协议(ipc_protocol.hpp)只保证"一帧完整、帧头严格校验";IPC-001 中
// FactsSnapshot 载荷是不透明字节(服务端只回"N bytes")。IPC-002 起把载荷
// 定义为两端约定的结构化 UTF-8 文本:
//   - 首行信封:CPOPFACTS/1(与帧协议版本相互独立,载荷内部另行版本化);
//   - 后续每行一条 key=value(行尾只允许 LF;CR 出现即整体拒绝);
//   - 键:ASCII 字母/数字/_/-,1..kMaxFactsKeyBytes 字节;
//   - 值:0..kMaxFactsValueBytes 字节 UTF-8 文本(允许空格与多字节字符,
//     禁止控制字节--保证回显与日志不被注入);
//   - 至多 kMaxFactsEntries 条;重复键整体拒绝(歧义即拒绝,不取"最后者胜");
//   - 不做宽松转换:任何一项违反立即整体拒绝(Validation),与帧级严格校验
//     同一哲学。
//
// 本层只负责“信封与语法 + v1 键语义白名单”（IPC-003），不解释单键值的业务含义：
// 哪些键被接受见 ValidateFactsV1Schema；键值的信任决策（会话/PID/用户 SID 裁决与
// 会话凭据，IPC-004~006）在 ipc_session.hpp，本层不参与。Serialize/Parse 互逆。

// 载荷信封(首行)。
inline constexpr std::string_view kFactsEnvelopeV1 = "CPOPFACTS/1";

// 会话凭据事实键(IPC-005):ASCII 字母/数字/_/-,1..kMaxFactsTokenBytes 字节。
// 属"内部凭据",不进入 FormatFactsSummary 回显(防泄漏到日志/应答)。
inline constexpr std::string_view kFactsTokenKey = "agent_token";
inline constexpr std::size_t kMaxFactsTokenBytes = 64;

// 单载荷最多事实条数。
inline constexpr std::size_t kMaxFactsEntries = 64;
// 键最大字节数(ASCII 可见子集)。
inline constexpr std::size_t kMaxFactsKeyBytes = 64;
// 值最大字节数(UTF-8 文本)。
inline constexpr std::size_t kMaxFactsValueBytes = 128;
// 服务端回执摘要上限(UTF-8 字节)。限制回显体积,保证应答载荷远低于帧上限。
inline constexpr std::size_t kFactsSummaryMaxBytes = 480;

// 单条事实:键 + UTF-8 文本值。
struct IpcFact {
    std::string key;
    std::string value;
};

// 严格解析 FactsSnapshot 载荷(UTF-8 文本)。失败返回 Validation(不宽松接受)。
[[nodiscard]] common::Result<std::vector<IpcFact>> ParseFactsV1(
    std::span<const std::byte> payload);

// 编码为 UTF-8 文本字节(与 ParseFactsV1 互逆)。输入违反契约返回 Validation。
[[nodiscard]] common::Result<std::vector<std::byte>> SerializeFactsV1(
    std::span<const IpcFact> facts);

// 紧凑回执摘要文本(UTF-8):"facts ok (N): k=v, ..."。超限时在整条事实边界
// 截断并追加 " ..."(不切分多字节字符)。仅用于应答/日志回显;输入须已通过
// Parse/Serialize 校验(无控制字节)。
[[nodiscard]] std::string FormatFactsSummary(std::span<const IpcFact> facts);

// 已注册数值键的通用取值(ACT-006,宿主/消费方访问器):线性查找 key,值非空十进制
// 无符号整数则返回其值;键缺失、值非十进制/为空返回 nullopt。本函数是通用语法访问,
// 不解释单键业务含义(键的语义由消费方按注册表说明解释,如 user_idle_seconds 为
// 距最近键鼠输入的秒数);调用方应确保输入已通过 ValidateFactsV1Schema(越界/畸形
// 值在 schema 层已拒,此处防御性返回 nullopt)。
[[nodiscard]] std::optional<std::uint32_t> NumericFactValue(
    std::span<const IpcFact> facts, std::string_view key) noexcept;

// ---------------------------------------------------------------------------
// Facts v1 键语义白名单（schema）。IPC-003：在语法契约（Parse/Serialize）之上
// 定义“哪些键被接受、值如何解释”。本层属应用语义，服务端据此把关；键值的
// 信任决策（会话/PID/用户 SID 裁决与共享凭据）在 ipc_session.hpp（IPC-004~006
// 已落地）。
//
// v1 已注册键(均可选,但整份载荷必须至少含一条已注册的**非凭据**事实):
//   - client_pid           无符号十进制整数(自报进程 ID,仅供参考,不替代服务端身份)
//   - memory_total_mb      无符号十进制整数,>= 1
//   - memory_available_mb  无符号十进制整数,>= 0;与 memory_total_mb 同现时须 <= total
//   - memory_load_percent  无符号十进制整数,0..100(内存负载,GlobalMemoryStatusEx 口径)
//   - user_idle_seconds    无符号十进制整数(ACT-005:上报方当前会话距最近键鼠输入的
//                          秒数,GetLastInputInfo 口径,锁屏/断开时输入时钟冻结自然增长;
//                          查询失败时上报方应省略该键——不伪装,本层不解释)
//   - foreground_pid       无符号十进制整数 >= 1(IPC-019:上报方会话当前前台窗口所属
//                          进程 pid,GetForegroundWindow 只读;无前台窗口或查询失败时
//                          上报方应省略该键——不伪装)
//   - observer             UTF-8 文本,非空(上报方自述,用于调试/审计)
//   - agent_token          ASCII 会话凭据(IPC-005,1..kMaxFactsTokenBytes);服务端
//                          Options.expectedToken 配置时才校验(缺失/不匹配回
//                          Error(AuthFailed));永不回显(见 FormatFactsSummary)
//
// 约束:未知键、键值非十进制整数、数值越界、observer/agent_token 为空或超限、整份不含
// 任何非凭据已注册键均整体拒绝(Validation,不宽松接受)。扩展 schema 必须同步更新本
// 注册表与对应测试。
[[nodiscard]] common::Result<void> ValidateFactsV1Schema(
    std::span<const IpcFact> facts);

} // namespace optimizer::ipc
