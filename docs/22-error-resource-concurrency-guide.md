# Windows 错误、资源所有权与并发处理详解

> **目标**：建立本项目所有模块共用的底层正确性模型。

---

## 1. 错误域不能混用

| 错误域 | 常见返回 | 成功条件 | 获取说明 |
|---|---|---|---|
| Win32 | `BOOL` | 非 FALSE | 失败后立即 `GetLastError` |
| Win32 HANDLE | `HANDLE` | 依 API 为 `nullptr` 或非 `INVALID_HANDLE_VALUE` | 查具体文档 |
| HRESULT | `HRESULT` | `SUCCEEDED(hr)` | `_com_error`/FormatMessage 或专用信息 |
| PDH | `PDH_STATUS` | `ERROR_SUCCESS` | 不使用 `GetLastError` |
| NTSTATUS | `NTSTATUS` | `NT_SUCCESS(status)` | 保留原值，可转 DOS error |
| C++ | 异常/error_code | 依接口 | 顶层转换 |

`GetLastError` 是线程局部的，而且成功 API 也可能修改它。正确模式：

```cpp
if (!SomeWin32Api()) {
    const DWORD error = ::GetLastError(); // 立即捕获
    return Error::FromWin32(error, "SomeWin32Api");
}
```

不要在失败和 `GetLastError` 之间插入日志、字符串分配或其他 API。

---

## 2. 项目统一错误类型

建议：

```cpp
enum class ErrorDomain { Win32, HResult, Pdh, NtStatus, Cpp, Validation };

struct Error {
    ErrorDomain domain;
    std::uint64_t code;
    std::string operation;
    std::wstring message;
    std::source_location location;
};

template<class T>
using Result = /* expected<T, Error>；C++20 可自实现轻量版本 */;
```

错误必须保留原始域和码。将 NTSTATUS 转 Win32 只用于展示，不能丢弃原 NTSTATUS。

### 2.1 查询接口

错误示例：

```cpp
double GetGpuLoad(); // 0 是空闲还是失败？
```

推荐：

```cpp
Result<MetricSample> QueryGpuLoad();
```

并区分 Valid、WarmingUp、Unsupported、NotFound 和 Error。

---

## 3. 资源配对表

| 获取 | 释放 | 常见错误 |
|---|---|---|
| `CreateFile/OpenProcess/CreateEvent` | `CloseHandle` | 忘关、双关 |
| `OpenSCManager/OpenService/CreateService` | `CloseServiceHandle` | 错用 CloseHandle |
| `PowerGetActiveScheme` | `LocalFree` | 错用 delete/free |
| `FormatMessage(ALLOCATE_BUFFER)` | `LocalFree` | 缓冲泄漏 |
| `SHGetKnownFolderPath` | `CoTaskMemFree` | 错用 LocalFree |
| COM `Create*/QueryInterface` | `Release`/`ComPtr` | 裸指针泄漏 |
| `PdhOpenQuery` | `PdhCloseQuery` | query 泄漏 |
| `PdhAddCounter` | `PdhRemoveCounter` 或 query 关闭 | 重复释放 |
| `StartTrace` | `ControlTrace(...STOP)` | 遗留 ETW session |
| `OpenTrace` | `CloseTrace` | consumer 句柄泄漏 |
| `RegOpenKeyEx` | `RegCloseKey` | 错用 CloseHandle |

正式 Common 不应只有一个假设所有资源都用 `CloseHandle` 的 AutoHandle；应使用 traits。

---

## 4. 伪句柄和借用资源

- `GetCurrentProcess()`、`GetCurrentThread()` 返回伪句柄，不关闭；
- callback 参数中的句柄通常是借用，除非文档明确转移所有权；
- `GetModuleHandleW` 返回模块借用句柄，不调用 `FreeLibrary`；
- `LoadLibraryW` 返回拥有句柄，需要 `FreeLibrary`；
- `GetProcAddress` 返回函数地址，不单独释放。

所有封装构造函数要明确 `adopt` 还是 `borrow`，避免误接管。

---

## 5. 生命周期与停止

模块状态：

```text
Created → Initialized → Starting → Running → Stopping → Stopped
                         ↘ Failed ↗
```

规则：

- Initialize 失败必须回滚已创建资源；
- Start 只有 Initialized/Stopped 可调用；
- Stop 可重复，且唤醒等待线程；
- Shutdown 先 Stop，再逆序释放依赖；
- Worker 线程不能访问已经释放的 event/query/config；
- 析构调用 noexcept Shutdown，但业务代码仍应显式 Shutdown。

---

## 6. 回调与锁

错误模式：

```cpp
std::lock_guard lock(mutex_);
for (auto& callback : callbacks_) callback(event); // 可能重入并死锁
```

正确模式：

```cpp
std::vector<Callback> copy;
{
    std::lock_guard lock(mutex_);
    copy = callbacks_;
}
for (auto& callback : copy) callback(event);
```

进一步要求：

- 说明 callback 执行线程；
- 订阅对象负责在销毁前取消订阅；
- 事件顺序和重复语义写入契约；
- callback 异常在分发边界捕获；
- 高吞吐路径考虑队列，但先保证正确再优化。

---

## 7. 进程身份与 PID 重用

PID 会重用。任何跨时间保存的 PID 必须配合创建时间或 generation：

```text
ProcessIdentity = { pid, creationTime }
```

执行动作前重新打开句柄并校验创建时间；不匹配则视为目标已退出，绝不作用于新进程。

---

## 8. TOCTOU 与权限变化

“刚刚查询到存在”不代表后续操作时仍存在。对进程、文件、窗口均应接受：

- 查询与执行之间目标退出；
- 权限/所有者变化；
- 文件被替换；
- 窗口句柄复用；
- 电源/会话状态变化。

处理原则是操作对象句柄而不是仅靠名称，并把失败视为正常竞争条件，不无限重试。

---

## 9. 内存和缓冲区

- 所有 Win32 结构先零初始化，并设置要求的 `cbSize/dwLength`；
- 长度单位明确是字节还是字符；
- 两阶段查询先取长度，再分配，再处理长度变化重试；
- 乘法前检查溢出；
- 不把 64 位指针/计数塞进 DWORD；
- 未文档化结构必须检查返回长度至少达到读取字段边界。

---

## 10. 审查清单

每个 API 调用逐项检查：

1. 头文件和库；
2. 目标 Windows 版本；
3. 调用线程/COM apartment/IRQL（未来驱动）；
4. 输入单位、编码、结构长度；
5. 成功条件；
6. 正确错误域；
7. 获取和释放；
8. 权限与降级；
9. 阻塞、超时和取消；
10. 并发与回调重入；
11. 系统级副作用和恢复；
12. 日志是否足够且不泄露隐私。
