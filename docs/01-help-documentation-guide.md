# Windows API 帮助文档查阅指南

> **本文档是 Windows API 查阅导航。** 任何 API 不会用、参数看不懂、行为不确定时，先按本文建立 API 卡片，再查官方文档。项目的 AI 协作、任务分工和学习完成度规则见 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md)。初学者不需要在完全没有候选 API 的情况下盲猜；AI 应先给出候选、差异和安全边界。

---

## 一、官方文档入口（优先级排序）

### 1.1 Microsoft Learn（首选）
- **地址**：https://learn.microsoft.com/zh-cn/windows/win32/
- **特点**：中文支持好、示例完整、持续更新
- **使用方式**：
  - 直接搜索 API 名称，如 `NtSetSystemInformation`
  - 或按模块浏览：`API 参考 → Windows API → 系统服务`

### 1.2 Windows API 参考索引
- **地址**：https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-api-list
- **特点**：按字母排序的完整 API 列表，适合已知 API 名但不确定模块时

### 1.3 WDK 文档（真正驱动开发时使用）
- **地址**：https://learn.microsoft.com/en-us/windows-hardware/drivers/
- **特点**：WDF、IRP、IRQL、驱动签名和调试
- **当前边界**：本项目当前不实现 `.sys` 驱动；调用用户态 `Nt*` 不等于驱动开发

### 1.4 NT 内部资料（辅助来源，不作为唯一依据）
- **地址**：http://undocumented.ntinternals.net/
- **特点**：可辅助理解部分 Nt/Zw 接口和历史结构
- **注意**：非 Microsoft 支持契约，可能过期或不完整。正式决策还需结合 SDK 头、符号、目标 OS 能力探测、隔离测试和安全失效设计

---

## 二、快速定位 API 的方法

### 2.1 已知 API 名称
```
搜索格式：site:learn.microsoft.com <API名称>
示例：site:learn.microsoft.com SetPriorityClass
```

### 2.2 已知功能但不知道 API
```
搜索格式：site:learn.microsoft.com "功能描述" win32 api
示例：site:learn.microsoft.com "set process priority" win32 api
```

### 2.3 查看头文件定义（最准确）
- 安装 Visual Studio 后，头文件位于：
  ```
  C:\Program Files (x86)\Windows Kits\10\Include\<版本>\um\    (用户态)
  C:\Program Files (x86)\Windows Kits\10\Include\<版本>\km\    (内核态)
  ```
- 使用 VS Code 的 "Go to Definition" 或 `grep` 搜索

### 2.4 查看导入库
- 确定需要链接的 `.lib` 文件：
  ```
  C:\Program Files (x86)\Windows Kits\10\Lib\<版本>\um\<arch>\
  ```

---

## 三、文档阅读技巧

### 3.1 必看字段（按优先级）

| 字段 | 为什么重要 | 示例 |
|------|----------|------|
| **Requirements** | 告诉你需要包含哪个头文件、链接哪个 lib | `Windows.h`, `Kernel32.lib` |
| **Parameters** | 每个参数的含义、取值范围、特殊值 | `dwPriorityClass` 的枚举值 |
| **Return value** | 成功/失败的判断方式，错误码查询 | `TRUE`/`FALSE`, `GetLastError()` |
| **Remarks** | 隐藏的行为细节、副作用、线程安全 | 是否需要管理员权限 |
| **See also** | 相关 API，构建知识网络 | 同类功能的替代方案 |

### 3.2 返回值处理模式
```cpp
// 模式一：BOOL 返回值
// 本项目禁止 REALTIME_PRIORITY_CLASS，正式默认上限为 ABOVE_NORMAL。
if (!SetPriorityClass(hProcess, ABOVE_NORMAL_PRIORITY_CLASS)) {
    DWORD err = GetLastError();
    // 处理错误
}

// 模式二：HANDLE 返回值
HANDLE hToken = OpenProcessToken(...);
if (hToken == NULL) {
    DWORD err = GetLastError();
    // 处理错误
}

// 模式三：NTSTATUS 返回值（Nt 系列）
NTSTATUS status = NtSetSystemInformation(...);
if (!NT_SUCCESS(status)) {
    // 使用 RtlNtStatusToDosError 转换或直接处理 NTSTATUS
}
```

### 3.3 权限要求速查
- 文档中搜索 **"Required privilege"** 或 **"SE_***_NAME"**
- 常见权限：
  - `SE_DEBUG_NAME`：调试其他进程
  - `SE_INCREASE_QUOTA_NAME`：提升进程优先级
  - `SE_SHUTDOWN_NAME`：关机/重启
  - `SE_LOAD_DRIVER_NAME`：加载驱动

---

## 四、调试与验证工具

### 4.1 运行时验证
| 工具 | 用途 | 获取方式 |
|------|------|---------|
| **Process Monitor (ProcMon)** | 查看进程的文件/注册表/API 调用 | Sysinternals |
| **Process Explorer** | 查看进程句柄、DLL、线程、性能计数器 | Sysinternals |
| **DebugView** | 捕获 `OutputDebugString` 输出 | Sysinternals |
| **Performance Monitor** | 系统性能计数器可视化 | Windows 自带 |
| **GPUView** | GPU 调度、D3D 调用分析 | Windows SDK |

### 4.2 静态分析
- **IDA Pro / Ghidra**：反编译查看系统 DLL 内部实现
- **dumpbin / objdump**：查看导入导出表
  ```bash
  dumpbin /imports kernel32.dll
  ```

---

## 五、常见陷阱与避坑指南

### 5.1 头文件版本问题
```cpp
// 使用最新 Windows SDK 时，定义目标版本
#define _WIN32_WINNT 0x0A00  // Windows 10
#define WINVER 0x0A00
#include <Windows.h>
```

### 5.2 Unicode / ANSI 混淆
```cpp
// 统一使用 Unicode 版本
#define UNICODE
#define _UNICODE
#include <Windows.h>
// 所有 API 自动映射到 W 版本（如 CreateFileW）
```

### 5.3 32/64 位指针截断
```cpp
// 错误：DWORD 无法保存 64 位指针
DWORD ptr = (DWORD)GetProcAddress(...);  // ❌

// 正确：使用 ULONG_PTR
ULONG_PTR ptr = (ULONG_PTR)GetProcAddress(...);  // ✅
```

### 5.4 句柄泄漏
```cpp
// 每个 Create/Open 必须有对应的 Close
HANDLE hProcess = OpenProcess(...);
if (hProcess) {
    // ... 使用 ...
    CloseHandle(hProcess);  // 必须！
}
```

---

## 六、推荐的学习路径（文档层面）

```text
第 1 阶段：AI 给出问题模型、候选 API 和 L0/L1 安全示范
          ↓ 学习者观察返回值、错误和资源生命周期
第 2 阶段：按 API 卡片精读 Requirements / Return value / Remarks
          ↓ 学习者补全 L2 骨架，不盲猜也不机械手抄
第 3 阶段：学习错误域、权限、所有权和失败注入
          ↓ 学习者完成一个额外测试和一个安全变体
第 4 阶段：比较替代 API，并在不看答案时重写关键小函数
          ↓ 用解释、测试和调试结果证明知识转化
```

每次新 API 的卡片字段、教学级别和绿/黄/红边界统一见 docs/00。

---

## 七、速查表：本项目涉及 API 的文档直达链接

> 以下链接截至 2026-08-08 有效，如失效请使用上述搜索方法。

### 内存管理
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `NtSetSystemInformation` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntsetsysteminformation) | `winternl.h` | `ntdll.lib` |
| `NtQuerySystemInformation` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation) | `winternl.h` | `ntdll.lib` |
| `EmptyWorkingSet` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-emptyworkingset) | `psapi.h` | `psapi.lib` |
| `SetProcessWorkingSetSize` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-setprocessworkingsetsize) | `memoryapi.h` | `kernel32.lib` |

### 电源管理
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `PowerCreateRequest` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/powerbase/nf-powerbase-powercreaterequest) | `powerbase.h` | `kernel32.lib` |
| `PowerSetRequest` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-powersetrequest) | `winbase.h` | `kernel32.lib` |
| `SetThreadExecutionState` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate) | `winbase.h` | `kernel32.lib` |

### 进程/线程调度
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `SetPriorityClass` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass) | `processthreadsapi.h` | `kernel32.lib` |
| `SetThreadPriority` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) | `processthreadsapi.h` | `kernel32.lib` |
| `SetThreadPriorityBoost` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriorityboost) | `processthreadsapi.h` | `kernel32.lib` |

### GPU / DXGI
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `CreateDXGIFactory2` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-createdxgifactory2) | `dxgi1_3.h` | `dxgi.lib` |
| `IDXGIAdapter::EnumOutputs` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-enumoutputs) | `dxgi.h` | `dxgi.lib` |
| `D3D11CreateDevice` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice) | `d3d11.h` | `d3d11.lib` |

### 性能监控
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `PdhOpenQuery` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhopenquery) | `pdh.h` | `pdh.lib` |
| `PdhAddCounter` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddcounter) | `pdh.h` | `pdh.lib` |
| `PdhCollectQueryData` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhcollectquerydata) | `pdh.h` | `pdh.lib` |
| `StartTrace` / `EnableTraceEx2` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/) | `evntrace.h` | `advapi32.lib` |

### ETW (Event Tracing for Windows)
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `EventRegister` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/evntprov/) | `evntprov.h` | `advapi32.lib` |
| `StartTrace` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttrace) | `evntrace.h` | `advapi32.lib` |

### 进程监控
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `CreateToolhelp32Snapshot` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot) | `tlhelp32.h` | `kernel32.lib` |
| `Process32First` / `Process32Next` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/) | `tlhelp32.h` | `kernel32.lib` |
| `EnumWindows` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumwindows) | `winuser.h` | `user32.lib` |
| `GetWindowThreadProcessId` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowthreadprocessid) | `winuser.h` | `user32.lib` |
| `RegisterWindowMessage` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerwindowmessage) | `winuser.h` | `user32.lib` |

### 服务/守护进程
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `StartServiceCtrlDispatcher` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-startservicectrldispatcher) | `winsvc.h` | `advapi32.lib` |
| `RegisterServiceCtrlHandlerEx` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-registerservicectrlhandlerex) | `winsvc.h` | `advapi32.lib` |
| `SetServiceStatus` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-setservicestatus) | `winsvc.h` | `advapi32.lib` |

### 令牌/权限
| API | 文档链接 | 头文件 | 库文件 |
|-----|---------|--------|--------|
| `OpenProcessToken` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocesstoken) | `processthreadsapi.h` | `advapi32.lib` |
| `AdjustTokenPrivileges` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges) | `securitybaseapi.h` | `advapi32.lib` |
| `LookupPrivilegeValue` | [MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lookupprivilegevalue) | `winbase.h` | `advapi32.lib` |

---

## 八、文档更新与维护

- 本文档随项目迭代更新；
- 新增 API 时，务必补充到第七节速查表或对应模块的 API 卡片；
- API 卡片必须覆盖失败值、错误域、最小权限、所有权、释放函数和副作用；
- AI 不得只给函数名或一段省略错误处理的调用代码；
- 学习者应逐步从“阅读卡片”过渡到“自己填写卡片”；
- 发现文档链接失效时，及时替换为最新链接。

---

*文档版本：v1.0 | 创建日期：2026-08-08 | 维护者：项目开发者*
