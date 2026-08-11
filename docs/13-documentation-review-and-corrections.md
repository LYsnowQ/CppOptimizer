# 项目文档审查、风险修正与落地建议

> **状态**：v1.0（2026-08-09）  
> **目的**：统一修正文档 01～12 中影响实现、安全性和可验证性的关键问题。

## 1. 总体结论

> 当前程序属于用户态系统编程，不是内核驱动。Native API 与内核态的详细区别见 [20-user-native-kernel-boundary-learning-guide.md](20-user-native-kernel-boundary-learning-guide.md)。正式工程、安全和测试规则见 docs/21～24。AI 与学习者的协作必须遵循 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md)，不再以“完整代码写入 Markdown 后机械手抄”为默认流程。

当前文档已经给出了模块边界，但仍偏“概念方案”，部分能力描述超过了 Windows API 的实际保证。项目应先做**可观测、可回滚、默认保守**的 MVP，再通过基准测试决定是否开放高风险动作。

### 1.1 推荐 MVP

1. ConfigManager + Logger + Common；
2. MetricsCollector + ProcessWatcher；
3. PolicyEngine（仅状态机、冷却和审计日志）；
4. PowerLocker（只使用电源请求，暂不切换全局电源计划）；
5. PriorityBooster（只允许 `ABOVE_NORMAL_PRIORITY_CLASS` / `HIGH_PRIORITY_CLASS`，禁止实时优先级）；
6. MemoryTuner 仅保留状态查询和手动实验入口，默认不周期清理；
7. GPU 心跳默认关闭，作为实验功能独立评测。

## 2. 必须修正的技术结论

### 2.1 Power Request 不会锁定 CPU/GPU P-State

`PowerSetRequest(PowerRequestExecutionRequired)` 的公开语义是阻止系统因空闲而睡眠；`DisplayRequired` 阻止显示器空闲关闭。它们不保证 CPU/GPU 维持最高频率。电源计划切换是全局系统行为，必须保存原计划、引用计数、崩溃恢复，并默认关闭。

### 2.2 GPU 心跳不是“零资源竞争”

任何 D3D workload 都会消耗 GPU 时间、功耗和温度预算，可能与游戏竞争并降低 boost 空间。该模块必须：

- 默认关闭并标记 Experimental；
- 游戏 GPU 已有负载时自动停止；
- 电池供电、温度过高、远程桌面、软件适配器时禁止启动；
- 设置硬上限和 watchdog；
- 通过 A/B 数据证明收益，否则删除。

### 2.3 Standby List 通常是有价值的缓存

清空 Standby List 可能增加后续硬缺页和加载时间，并不等同于“释放被浪费的内存”。因此：

- 禁止默认定时清理；
- 游戏启动/加载期间禁止清理；
- `FlushModifiedList` 默认永久禁用；
- 只有在明确内存压力、经过冷却且用户显式允许时执行 Light；
- 必须记录清理前后可用内存、硬缺页率和磁盘 I/O。

### 2.4 服务 Session 0 限制

Windows 服务位于 Session 0，不能可靠地直接完成交互用户窗口枚举、前台窗口判断和用户输入检测。正式服务化建议采用：

```text
Windows Service（高权限、系统动作）
        ↕ 命名管道 + ACL + 消息版本
Per-user Agent（窗口、前台、用户配置、托盘）
```

MVP 应优先使用控制台/托盘进程，服务化放到后续阶段。

### 2.5 删除“启动器 DLL 注入”部署方式

该方式与“不注入游戏/启动器、反作弊友好”的非目标冲突，应从总体架构移除。允许的形态仅为独立 EXE、托盘 Agent 和可选 Windows Service。

### 2.6 进程句柄不能以裸 `HANDLE` 放入可复制结构体

`ProcessInfo::safeHandle` 会造成所有权不清、重复关闭或泄漏。应返回 PID 和只读快照；执行器需要句柄时按最小权限临时打开，或使用共享 RAII 句柄类型。

### 2.7 PDH 本地化与实例名问题

中文 Windows 上英文路径需优先使用 `PdhAddEnglishCounterW`。`\Process(name)` 实例名可能出现 `name#1`，PID 映射应使用 `ID Process` 计数器或直接通过进程 API 采样，不能只依赖实例名。

### 2.8 ETW 不应作为第一版硬依赖

内核 ETW 会话存在权限、会话名冲突、事件 schema 和系统版本差异。第一版使用 Toolhelp 轮询 + 进程句柄等待即可；ETW 作为可选低延迟后端。

## 3. 统一工程约束

### 3.1 生命周期

所有运行模块采用：

```text
Created → Initialized → Running → Stopping → Stopped
```

- `Initialize` 与 `Shutdown` 成对；
- `Start` 与 `Stop` 成对；
- 重复 `Stop/Shutdown` 必须安全；
- 析构只做无异常清理；
- 回调不得在持有内部互斥锁时执行。

### 3.2 时间与单位

- 持续时间使用 `std::chrono`；
- 内部时钟使用 `std::chrono::steady_clock`；
- 日志时间使用 UTC `system_clock`；
- 字节字段使用 `std::uint64_t`，百分比统一为 `0.0～100.0`；
- 不用裸 `uint64_t timestamp` 表达含义不明确的时间。

### 3.3 错误模型

每次动作至少记录：模块、动作、目标、结果、Win32/PDH/HRESULT/NTSTATUS 原始错误、转换后的说明、耗时。查询接口不得用 `0` 同时表达“真实值为零”和“查询失败”；建议返回结果结构或 `std::optional`。

### 3.4 回滚和租约

有副作用的优化必须采用“租约”模型：

- Acquire 返回令牌；
- 同一动作支持多个请求者引用计数；
- Release 只释放自己的租约；
- 进程退出、停止服务、异常路径均恢复原状态；
- 全局设置写入恢复记录，启动时检查上次异常退出。

## 4. 测试门禁

| 类别 | 最低要求 |
|---|---|
| 单元测试 | 配置校验、规则迟滞、冲突解决、引用计数、状态机 |
| 集成测试 | 启停 100 次、配置热重载、目标进程快速启动退出、权限不足 |
| 稳定性 | 标准用户与管理员、睡眠/唤醒、锁屏、用户注销、24 小时运行 |
| 性能 | 优化器自身 CPU、工作集、句柄数、线程数、唤醒次数 |
| 效果 | 相同场景配对 A/B，报告帧时间 P50/P95/P99、1% low、功耗和温度 |

禁止只用平均 FPS 宣称优化有效。任何默认开启的动作都必须证明在目标设备上不存在显著负优化。

## 5. 文档维护规则

- 设计文档描述“为什么、边界和契约”；
- `include/<module>/*.md` 描述公共类型和接口；
- `source/<module>/*.md` 描述实现步骤、API、线程、安全、测试和验收；
- 示例代码必须区分“可编译基线”和“伪代码”；
- 未实现能力标记 `Planned`，实验能力标记 `Experimental`。
