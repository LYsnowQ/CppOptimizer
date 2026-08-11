# 模块设计文档：电源锁定器 (PowerLocker)

> **所属层**：Layer 3 - 应急响应层  
> **模块ID**：MOD-PWR-001  
> **状态**：大纲阶段  
> **学习协作建议**：Power Request 的第一次租约模式使用 L1/L2。AI 主导句柄、引用计数、恢复和系统睡眠语义；学习者实现 reason、fake、局部 API 调用和重复 Acquire/Release 测试。绿色区是 fake 与状态查询，黄色不变量是配对释放、幂等和引用计数；红色区是把 Power Request 宣称为锁频，以及默认切换全局电源计划。

---

## 一、模块概述

### 1.1 职责
- 创建电源请求（Power Request），阻止系统进入睡眠/休眠
- 通过公开 Power Request 阻止空闲睡眠/熄屏；**不承诺锁定 CPU/GPU P-State**
- 可选电源计划切换作为独立、默认关闭的全局动作实现
- 游戏退出后自动释放电源请求
- 支持显示器状态控制（防止游戏时熄屏）

### 1.2 核心原理

Windows 电源管理通过电源请求（Power Request）机制工作。该机制表达的是睡眠/显示需求，不是处理器或 GPU 频率锁：
- `PowerRequestExecutionRequired`：告诉系统"我正在执行重要任务，不要睡眠"
- `PowerRequestDisplayRequired`：告诉系统"我需要显示器保持开启"
- `SetThreadExecutionState`：传统 API，功能类似但粒度较粗

电源请求是引用计数的，创建和关闭必须成对出现。

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::power {

enum class PowerLockType {
    ExecutionRequired,   // 阻止睡眠
    DisplayRequired,     // 阻止熄屏
    // 高性能电源计划不属于 Power Request 类型，使用独立控制器
};

class PowerLocker {
public:
    bool Initialize();
    bool AcquireLock(PowerLockType type, const std::wstring& reason);
    bool ReleaseLock(PowerLockType type);
    void ReleaseAll();
    bool IsLocked(PowerLockType type) const;
    // 电源计划切换由独立 PowerSchemeController 提供，默认关闭。

private:
    // PowerRequest 句柄管理
    // RAII 封装
};

} // namespace optimizer::power
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│           PowerLocker                   │
├─────────────────────────────────────────┤
│  ┌─────────────────────────────────┐   │
│  │      请求管理器 (Request Manager) │   │
│  │  map<PowerLockType, HANDLE>     │   │
│  │  PowerCreateRequest()           │   │
│  │  → PowerSetRequest()            │   │
│  │  → PowerClearRequest()          │   │
│  │  → CloseHandle()                │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │  可选 PowerSchemeController      │   │
│  │  - 与 PowerLocker 独立            │   │
│  │  - 默认关闭                       │   │
│  │  - 保存/恢复原始计划              │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │      安全释放器 (RAII Guard)      │   │
│  │  - 析构时自动 ReleaseAll()        │   │
│  │  - 异常安全保证                   │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 电源请求创建流程
```cpp
REASON_CONTEXT context;
context.Version = POWER_REQUEST_CONTEXT_VERSION;
context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
context.Reason.SimpleReasonString = L"Game optimization in progress";

HANDLE hRequest = PowerCreateRequest(&context);
if (hRequest == INVALID_HANDLE_VALUE) {
    // GetLastError()；不能继续 PowerSetRequest
}
if (!PowerSetRequest(hRequest, PowerRequestExecutionRequired)) {
    // 记录错误并 CloseHandle(hRequest)
}

// 清理
PowerClearRequest(hRequest, PowerRequestExecutionRequired);
PowerClearRequest(hRequest, PowerRequestDisplayRequired);
CloseHandle(hRequest);
```

### 4.2 电源计划切换
```cpp
// 高性能计划 GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
// 平衡计划 GUID: 381b4222-f694-41f0-9685-ff5bb260df2e
// 节能计划 GUID: a1841308-3541-4fab-bc81-f71556f20b4a

GUID* activeScheme;
PowerGetActiveScheme(NULL, &activeScheme);
// 保存原始计划；PowerGetActiveScheme 分配的指针必须 LocalFree
// 该全局动作默认关闭，需租约、引用计数和异常退出恢复记录

GUID highPerformance = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a,0x85,0xa6,0xe2,0x3a,0x8c,0x63,0x5c}};
PowerSetActiveScheme(NULL, &highPerformance);
```

### 4.3 异常安全
```cpp
class AutoPowerRequest {
    HANDLE handle_ = NULL;
public:
    explicit AutoPowerRequest(const REASON_CONTEXT& ctx) {
        handle_ = PowerCreateRequest(const_cast<REASON_CONTEXT*>(&ctx));
    }
    ~AutoPowerRequest() {
        if (handle_) {
            PowerClearRequest(handle_, PowerRequestExecutionRequired);
            PowerClearRequest(handle_, PowerRequestDisplayRequired);
            CloseHandle(handle_);
        }
    }
    AutoPowerRequest(const AutoPowerRequest&) = delete;
    AutoPowerRequest& operator=(const AutoPowerRequest&) = delete;
    AutoPowerRequest(AutoPowerRequest&& other) noexcept { /* ... */ }
};
```

---

## 五、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| PolicyEngine | 依赖 | 接收锁定/解锁指令 |
| ProcessWatcher | 依赖 | 游戏退出触发自动释放 |
| ConfigManager | 依赖 | 读取电源计划配置 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
