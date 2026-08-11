# 模块设计文档：MemoryTuner 第一阶段

> **模块 ID**：MOD-MEM-001  
> **命名空间**：`optimizer::memory`  
> **状态**：Baseline（仅 R0 只读观测）  
> **技术层级**：Windows x64 Ring 3、文档化 Win32 API  
> **教学级别**：L2 骨架补全

## 1. 第一阶段目标

本阶段只建立可靠的物理内存状态快照，不实施任何“清理”动作。模块回答：

- 系统物理内存总量是多少；
- 当前可用物理内存是多少；
- 两者相减得到的已用物理内存是多少；
- Windows 报告的内存负载百分比是多少；
- 快照在单调时钟上的采样时刻是什么。

先把值、单位、失败语义和测试边界建立正确，才有资格讨论后续指标或实验。

## 2. 职责与非职责

### 职责

- 使用 `GlobalMemoryStatusEx` 执行一次只读查询；
- 用 `Result<MemoryStatus>` 区分有效值和错误；
- 校验总量、可用量和百分比不变量；
- 保持容量字段为 `uint64_t` 字节；
- 提供无需调用 Windows API 的纯逻辑构造函数。

### 非职责

- 不清理 Standby List 或 Modified List；
- 不调用 `NtSetSystemInformation`；
- 不调整当前进程或其他进程工作集；
- 不启用 privilege，不要求管理员；
- 不启动线程，不做周期任务；
- 不根据单次快照自动作出优化决策；
- 不把“可用内存增加”定义为性能收益。

## 3. 公共契约

真实接口位于 `include/memory/memory_tuner.hpp`：

- `MemoryStatus` 是一个值类型，不持有 HANDLE 或指针；
- `BuildMemoryStatus(...)` 校验原始输入并生成快照；
- `QueryMemoryStatus()` 执行一次同步查询；
- 两个函数均无系统副作用，可由多个线程并发调用；
- `sampledAt` 使用 `steady_clock`，不用于显示墙上时间；
- Windows 的 `dwMemoryLoad` 与简单字节比例可能因取整和记账口径不同，不要求完全一致。

## 4. 学习协作卡

- **AI 负责**：公共契约、Win32 API 调用、错误域、不变量、工程接线和基础测试；
- **学习者负责（已完成）**：MEM-001 作答与合法边界理解（`available == total` 已按证据合并，不重复出题）；MEM-003 的复盘、VS 调试观察与绿色变体；
- **学习者独立函数（已完成基线）**：`FormatBytes` 与 `IsSnapshotFresh` 已作为工程基线实现，学习者通过复盘、变体与解释完成知识转化；
- **安全变体**：验证 `available == total` 或 `memoryLoadPercent == 100` 是合法输入；
- **升级到 L3 的证据**：能不看实现写出 `dwLength` 初始化、BOOL 失败判断、立即保存 `GetLastError`，并解释所有字段单位。

## 5. API 卡片：GlobalMemoryStatusEx

| 字段 | 内容 |
|---|---|
| 系统领域 | 系统内存状态 |
| 使用原因 | 公开、稳定、标准用户可用的一次性物理内存查询 |
| 替代方案 | `GetPerformanceInfo`、PDH；它们用于不同统计或持续采样，本阶段不需要 |
| 头文件 | Windows SDK：`windows.h` / `sysinfoapi.h` |
| 链接库 / DLL | `Kernel32.lib` / `Kernel32.dll` |
| 关键参数 | `MEMORYSTATUSEX*`，调用前设置 `dwLength` |
| 成功返回 | 非零 |
| 失败返回 | 零 |
| 错误域 | Win32 |
| GetLastError | 仅失败后立即读取 |
| 最小权限 | 标准用户，无需提升 |
| 资源所有权 | 无返回资源，调用方拥有结构体存储 |
| 阻塞和线程 | 短同步查询；无模块共享状态 |
| 系统副作用 | 无 |
| 常见陷阱 | 忘记 `dwLength`；把失败当全零；字节/MiB 混用；百分比范围误写为 0～1 |
| 官方文档关键词 | `GlobalMemoryStatusEx function`, `MEMORYSTATUSEX structure` |

## 6. 数据与不变量

```text
totalPhysicalBytes > 0
availablePhysicalBytes <= totalPhysicalBytes
usedPhysicalBytes = totalPhysicalBytes - availablePhysicalBytes
0 <= memoryLoadPercent <= 100
```

先检查 `available <= total` 再做无符号减法，避免下溢产生极大错误值。查询失败返回项目统一 `ErrorDomain::Win32`；输入不变量失败返回 `ErrorDomain::Validation`。

## 7. 分步学习路线

### Step 1：纯逻辑快照构造（本轮）

- 理解字节、百分比和 `steady_clock`；
- 测试正常输入和三个无效输入；
- 学习者补充一个合法边界测试。

### Step 2：一次 Win32 查询（本轮）

- 观察 `MEMORYSTATUSEX` 的长度字段；
- 判断 BOOL；
- 失败时立即保存 Win32 error；
- 将原始数据交给纯逻辑构造。

### Step 3：只读展示（已完成，2026-08-10）

- `FormatBytes`：二进制单位（KiB/MiB/GiB/TiB）显示，整数 tenths 取整（round-half-up），显示舍入不改变内部字节真值；
- `IsSnapshotFresh`：纯时效判断，`now - sampledAt <= maxAge` 边界包含，未来时间戳不判陈旧；
- `--status` 已接入单次只读快照，不加入轮询；
- 学习者待办：复盘两个纯函数的边界、VS 调试观察、完成一个绿色变体。

### Step 4：可测试平台边界（后续）

- 评估是否需要注入只读 backend；
- 模拟 Win32 查询失败；
- 仍不进入 Native 写或清理动作。

## 8. 自由调试边界

### 绿色

- 纯逻辑计算；
- 合法/非法边界测试；
- 只读格式化；
- 快照排序、时效判断；
- 人工对照任务管理器或 RAMMap。

### 黄色不变量

- 失败不能伪装成有效零值；
- Win32 失败码必须立即捕获；
- 内部容量单位始终为字节；
- 不强制 Windows 百分比等于自行计算比例；
- 不为一次查询引入线程或全局可写状态。

### 红色

- `NtSetSystemInformation`；
- Standby/Modified List purge；
- `EmptyWorkingSet`；
- privilege 或管理员自动提升；
- 周期自动清理；
- Moderate/Aggressive 清理等级；
- 日常主机上的 R3 实验。

## 9. 测试与调试

### 自动测试

- 正常输入正确计算 used bytes；
- total 为 0 时返回 Validation；
- available 大于 total 时返回 Validation；
- 百分比大于 100 时返回 Validation；
- `FormatBytes`：裸字节显示、单位切换（KiB/MiB/GiB）、tenths 取整（round-half-up）；
- `IsSnapshotFresh`：窗口内新鲜、边界包含、超窗陈旧、未来时间戳；
- 学习者补充至少一个绿色变体测试。

### 人工观察

在 `QueryMemoryStatus()` 设置断点，检查：

- `dwLength`；
- API BOOL 返回；
- total/available 数量级；
- 百分比范围；
- 派生 used bytes。

任务管理器与 RAMMap 只做数量级对照。由于采样时刻和统计口径不同，不要求完全相等。

## 10. Definition of Done

第一阶段完成必须同时满足：

- `.hpp/.cpp`、include/source 学习说明和工程文件同步；
- x64 MSVC Debug/Release 构建通过；
- 自动测试实际运行通过；
- 学习者完成一个额外测试；
- 学习者在调试器中观察成功路径；
- 能解释 API 成功值、失败值、错误域、所有权和副作用；
- 搜索确认真实源码没有 Native 写和内存清理调用。

MSVC x64 Debug/Release 构建与单元测试已通过（2026-08-10）；学习者完成复盘、VS 调试观察与变体后，第一阶段教学即可宣称完成。
