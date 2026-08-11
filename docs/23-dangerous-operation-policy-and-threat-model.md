# 危险操作策略与威胁模型

> **状态**：Security Baseline v1.0  
> **目的**：防止学习代码因权限、参数或异常退出影响系统稳定性。

---

## 1. 风险等级

| 等级 | 定义 | 示例 | 默认 |
|---|---|---|---|
| R0 只读 | 无系统状态修改 | 内存/CPU 查询、进程枚举 | 可开启 |
| R1 局部可逆 | 仅目标进程/当前会话，可立即恢复 | AboveNormal 优先级、Power Request | 配置开启 |
| R2 系统级可逆 | 影响全局，需要恢复记录 | 电源计划切换 | 默认关闭 |
| R3 系统级实验 | Native API、性能/稳定性风险 | Standby purge、GPU 合成负载 | 编译和运行双重关闭 |
| R4 内核/不可接受 | 驱动、Hook、内核内存修改 | `.sys`、SSDT Hook | 当前禁止 |

---

## 2. 危险操作许可

R2/R3 操作不得由普通 bool 直接调用，要求不可伪造的许可对象：

```cpp
class DangerousOperationPermit {
public:
    DangerousOperationPermit(const DangerousOperationPermit&) = delete;
private:
    friend class SafetyGate;
    explicit DangerousOperationPermit(OperationId id, Expiry expiry);
};
```

SafetyGate 只有在以下全部成立时签发短期许可：

- 编译期功能开启；
- 配置显式开启；
- 命令行显式确认；
- 权限满足；
- OS/能力探测通过；
- 非电池/非远程/非锁屏等环境门禁通过；
- 当前无关键压力；
- 冷却时间满足；
- 审计日志可用；
- permit 带操作类型、目标、过期时间和单次使用语义。

---

## 3. 命令行确认

禁止仅使用 `--force`。采用可读、动作特定参数：

```text
--experimental
--allow-native-memory-write
--memory-clean=light
--acknowledge-system-wide-side-effects
```

服务模式不接受临时危险命令；危险实验只允许前台控制台运行，避免无人值守。

---

## 4. 威胁主体

1. 错误/恶意配置文件；
2. 非管理员本地用户向高权限服务发送 IPC；
3. 路径替换、软链接/重解析点和不安全文件权限；
4. PID 重用导致作用于错误进程；
5. 畸形 IPC 长度、版本或枚举值；
6. 回调重入和竞态造成状态绕过；
7. 日志包含用户路径或敏感参数；
8. 崩溃导致系统全局状态遗留；
9. Windows 更新改变 Native API 行为；
10. 反作弊将高权限、进程操作或 GPU 行为判为可疑。

---

## 5. 安全控制

### 配置

- 用户配置目录 ACL 仅当前用户；
- 服务配置目录 ACL 仅管理员/SYSTEM；
- 配置先解析到临时对象、完整校验后原子替换；
- 不允许配置任意 DLL、脚本或命令执行路径；
- 路径正规化，必要时拒绝 reparse point。

### IPC

- 命名管道使用显式 SDDL；
- 校验客户端 token/session/PID；
- 消息头含 magic、版本、类型、长度和 request ID；
- 限制最大消息长度、队列和超时；
- 未知版本/枚举拒绝，不做宽松转换；
- Agent 只能提交事实快照，不能直接要求任意系统调用。

### 进程操作

- 白名单规则 + PID/创建时间；
- 最小权限；
- 不读写内存、不注入、不枚举敏感模块；
- 目标退出视为正常取消；
- 不自动对未知进程使用高权限。

### 恢复

- R2 操作在修改前写入小型恢复记录；
- 文件写入使用临时文件、flush、原子 rename；
- 启动时先检查并尝试恢复；
- 恢复失败进入 Safe Mode，拒绝新危险动作。

---

## 6. Safe Mode

以下任一情况进入 Safe Mode：

- 上次异常退出且恢复未确认；
- 配置无效；
- Native capability 探测异常；
- 审计日志不可用且请求 R2/R3；
- IPC 身份验证失败次数异常；
- 同一动作连续失败超过阈值；
- 检测到不支持的 OS/build。

Safe Mode 仅允许 R0 查询和恢复动作。

---

## 7. 安全审计日志

危险动作记录：

- operation_id、risk_level；
- 调用来源（console/policy/recovery）；
- 用户/session/process identity；
- 编译/配置/命令行门禁结果；
- 修改前状态；
- API 原始结果；
- 修改后验证；
- 释放/恢复结果；
- 持续时间。

日志中不记录 token、完整命令行秘密或用户输入内容。

---

## 8. 安全发布门禁

- R3 功能不得出现在默认配置；
- 未通过故障注入和恢复测试不得发布；
- Release 包不得包含测试驱动；
- 安装器不得默认要求管理员常驻；
- 版本说明明确 Experimental 风险；
- 每个危险模块提供一键禁用和诊断输出。
