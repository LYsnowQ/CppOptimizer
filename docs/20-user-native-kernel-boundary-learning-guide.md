# Windows 用户态、Native API 与内核态边界学习指南

> **读者**：第一次接触 Windows 底层开发的项目开发者  
> **状态**：正式学习基线 v1.1  
> **结论先行**：当前 CppOptimizer 主程序是 **Ring 3 用户态程序**。调用 `NtQuerySystemInformation` 或 `NtSetSystemInformation` 不会把代码变成内核驱动；它只是通过 `ntdll.dll` 发起系统调用，请求内核执行受控操作。  
> **学习方式**：本指南涉及高风险概念，默认由 AI 按 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md) 的 L0/L1 方式解释和示范；学习者优先完成 Probe、只读查询、fake、门禁测试和结果分析，不从零独立尝试真实 Native 写或驱动加载。

---

## 1. 三个容易混淆的层次

```text
CppOptimizer.exe（Ring 3 用户态）
        │
        ├─ Win32 API：Kernel32/User32/Advapi32/PDH/DXGI
        │      文档化、兼容契约较稳定，优先使用
        │
        ├─ Native API：Ntdll!Nt*/Rtl*
        │      仍由用户态调用，但部分接口/信息类未公开承诺兼容
        │
        └─ 系统调用边界
               ↓
Windows Kernel（Ring 0）
        ├─ 内存管理器、调度器、I/O 管理器
        ├─ 已签名微软/第三方驱动
        └─ 可选的 CppOptimizer 驱动（当前不存在，也不在 MVP 范围）
```

| 层次 | 代码运行位置 | 典型产物 | 崩溃影响 | 本项目状态 |
|---|---|---|---|---|
| Win32 用户态 | Ring 3 | EXE/DLL | 通常只影响当前进程 | 主体、优先 |
| Native API 调用 | 调用方仍在 Ring 3 | EXE/DLL | 通常只影响当前进程，但请求可能改变系统状态 | 少量 Experimental |
| 内核驱动 | Ring 0 | SYS | 错误可能导致 BSOD、数据损坏或安全漏洞 | 当前禁止进入 |

### 1.1 ETW、PDH、服务不是驱动

- ETW 可以消费内核 provider 的事件，但消费者仍可在用户态；
- PDH 是用户态性能数据接口；
- Windows Service 是 Session 0 中的用户态进程；
- 管理员权限不等于内核态；
- `SeDebugPrivilege` 等 token privilege 也不等于 Ring 0。

---

## 2. 为什么 Native API 仍然危险

危险不一定来自“代码运行在内核”，还可能来自接口契约和动作范围：

1. **信息类或结构未文档化**：Windows 更新可能改变结构布局或行为；
2. **系统级副作用**：清理 Standby、修改全局电源计划会影响其他进程；
3. **权限与策略差异**：标准用户、管理员、受限 token、企业策略下结果不同；
4. **错误处理复杂**：NTSTATUS、Win32、HRESULT、PDH_STATUS 不能混用；
5. **回滚困难**：进程崩溃时析构函数不会可靠执行，全局修改可能遗留。

因此，Native API 模块必须同时具备：运行时解析、版本/能力探测、默认关闭、显式授权、超时、审计、回滚和文档化替代路径。

---

## 3. 本项目允许与禁止的边界

### 3.1 当前允许

- 文档化 Win32 API 的只读查询；
- 最小权限 `OpenProcess`；
- PDH 指标采集；
- Toolhelp 进程枚举；
- Power Request；
- 受控的 AboveNormal/High 进程优先级；
- Native API 的只读能力探测；
- 用户显式授权后的实验性 Standby Light purge，且默认关闭。

### 3.2 当前禁止

- 新建或加载 `.sys` 驱动；
- 内核 Hook、SSDT Hook、对象回调、MiniFilter；
- DLL 注入、远程线程、进程内存读写；
- 绕过 PatchGuard、HVCI、驱动签名或反作弊；
- 修改内核内存、MSR、物理内存、设备寄存器；
- `REALTIME_PRIORITY_CLASS`；
- 自动 `FlushModifiedList`；
- 未经恢复设计的全局电源计划修改。

### 3.3 未来进入驱动开发的门槛

只有同时满足以下条件，才建立独立 driver solution：

1. 用户态 MVP 稳定且有真实需求无法通过公开 API 满足；
2. 完成 C/C++ 内存安全、IRQL、分页内存、同步、I/O 模型基础学习；
3. 使用专用测试机或虚拟机，启用快照；
4. 配置 WinDbg 内核调试和完整 dump；
5. 启用 Driver Verifier，理解其可能导致启动循环；
6. 驱动与用户态进程分仓或至少分 solution，接口使用版本化 IOCTL；
7. 完成威胁模型、ACL、输入长度校验、并发取消和卸载路径设计；
8. 不在安装有重要数据或日常游戏环境的主机首次测试。

---

## 4. 权限模型学习

### 4.1 四种常见运行上下文

| 上下文 | 特点 | 应采用的行为 |
|---|---|---|
| 标准用户 | 最小风险，部分跨进程操作失败 | 默认模式，功能降级 |
| 提升管理员 | token 含更多 privilege，但不自动启用 | 仅实验动作显式使用 |
| Windows Service | 用户态 Session 0，可能是 LocalSystem | 不直接处理交互桌面 |
| 内核驱动 | Ring 0，无普通进程隔离 | 当前禁止 |

### 4.2 最小权限原则

`OpenProcess` 只申请当前动作需要的权限。例如查询和设置优先级：

```cpp
PROCESS_QUERY_LIMITED_INFORMATION |
PROCESS_SET_INFORMATION |
SYNCHRONIZE
```

不要为了“省事”申请 `PROCESS_ALL_ACCESS`。请求权限越大，失败概率、攻击面和反作弊敏感度越高。

### 4.3 Privilege 的正确流程

```text
OpenProcessToken
 → LookupPrivilegeValueW
 → AdjustTokenPrivileges
 → 即使返回 TRUE 仍检查 ERROR_NOT_ALL_ASSIGNED
 → 完成动作
 → 恢复 PreviousState
 → CloseHandle(token)
```

正式封装应保存 `PreviousState` 并恢复，而不是永久改变当前进程 token 状态。

---

## 5. Native API 的正式封装要求

禁止业务模块在多处手写 `extern "C" Nt...` 声明。建立单一适配层，例如：

```text
include/platform/native_api.hpp
source/platform/native_api.cpp
```

适配层职责：

- 通过 `GetModuleHandleW(L"ntdll.dll")` + `GetProcAddress` 运行时解析；
- 保存函数指针，不直接静态链接实验符号；
- 对外返回项目统一错误类型；
- 提供 `IsSupported()`；
- 对系统版本和结构大小执行防御检查；
- 不把未文档化结构暴露给业务层；
- 所有修改操作要求 `DangerousOperationPermit`。

示例接口：

```cpp
struct NativeCapabilities {
    bool memoryListQuery = false;
    bool memoryListCommand = false;
};

class NativeApi {
public:
    static NativeApi& Instance();
    Result<NativeCapabilities> Probe() noexcept;
    Result<MemoryListSnapshot> QueryMemoryLists() noexcept;
    Result<void> PurgeLowPriorityStandby(const DangerousOperationPermit&) noexcept;
};
```

---

## 6. 第一次学习危险 API 的实验流程

1. 提交/保存当前代码，确保工作区可恢复；
2. 在非关键机器或虚拟机创建快照；
3. 只构建 x64 Debug；
4. 先运行 `--diagnose`，确认能力、权限、系统版本；
5. 运行只读查询，核对 RAMMap/Performance Monitor；
6. 导出实验前指标；
7. 使用三重确认参数，例如：

```text
--experimental
--allow-native-memory-write
--memory-clean=light
```

8. 单次执行，禁止循环；
9. 观察硬缺页、磁盘 I/O、事件日志和系统稳定性；
10. 导出实验后指标并重启验证无遗留；
11. 记录系统版本、硬件、命令、返回码和结论。

---

## 7. 驱动学习时必须掌握的概念（未来）

- IRQL 与可调用 API 限制；
- paged/nonpaged pool 与 NX；
- `WDFOBJECT` 生命周期和 parent-child 清理；
- IOCTL 的 METHOD_BUFFERED / DIRECT / NEITHER；
- 用户输入指针永远不可信；
- Remove Lock、取消、PnP/Power 状态；
- 内核同步与死锁；
- Driver Verifier、WinDbg、dump 分析；
- HVCI、签名、Secure Boot 和部署策略。

在这些概念具备前，不从本项目复制用户态 RAII/线程模型直接编写驱动；内核环境有不同运行库、异常、内存和同步约束。

---

## 8. 每次接触底层 API 的自问清单

- 代码究竟运行在 Ring 3 还是 Ring 0？
- API 是公开契约、半公开还是未文档化？
- 最坏副作用只影响当前进程，还是整个系统？
- 成功条件属于 BOOL/HANDLE/HRESULT/PDH_STATUS/NTSTATUS 哪一类？
- 分配者是谁，释放函数是什么？
- 权限不足时能否安全降级？
- 进程崩溃后如何恢复？
- Windows 更新后如何探测不支持，而不是直接调用？
- 是否有只读替代方案？
- 是否有测量证据证明值得承担风险？
