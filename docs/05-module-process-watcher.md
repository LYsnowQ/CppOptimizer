# 模块设计文档：进程生命周期监控器 (ProcessWatcher)

> **所属层**：Layer 1 - 监控感知层  
> **模块ID**：MOD-PRC-001  
> **状态**：大纲阶段  
> **学习协作建议**：Toolhelp 首次枚举使用 L1/L2，身份与竞态共同实现。AI 提供候选 API、最小权限、PID generation 和 callback 边界；学习者实现快照、匹配、差分和故障测试。绿色区是只读枚举/过滤，黄色不变量是拥有型快照句柄、PID + 创建时间、锁外 callback 和可停止轮询；红色区禁止 `PROCESS_ALL_ACCESS`、注入和游戏内存访问。

---

## 一、模块概述

### 1.1 职责
- 发现目标游戏进程的启动
- 跟踪目标进程的生命周期（运行中/退出/挂起）
- 提供进程句柄的安全获取（用于后续优先级调整等）
- 窗口状态检测（前台/全屏/最小化）

### 1.2 检测策略

| 策略 | 方法 | 精度 | 开销 | 适用场景 |
|------|------|------|------|---------|
| **进程名轮询** | `CreateToolhelp32Snapshot` | 中 | 低 | 备用/兜底 |
| **窗口标题匹配** | `EnumWindows` + `GetWindowText` | 高 | 低 | 主检测 |
| **ETW 进程事件** | `Microsoft-Windows-Kernel-Process` | 最高 | 极低 | 实时检测 |
| **WMI 事件订阅** | `__InstanceCreationEvent` | 高 | 中 | 备选 |

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::process {

enum class ProcessState {
    NotRunning, Starting, Running, Suspended, Exiting
};

struct ProcessInfo {
    DWORD pid;
    std::wstring processName;
    std::wstring windowTitle;
    ProcessState state;
    bool isForeground;
    bool isFullscreen;
    uint64_t generation; // 与 pid 共同标识一次进程生命周期，防止 PID 重用
    // 不在可复制快照中暴露 HANDLE；执行器按需以最小权限临时打开
};

class ProcessWatcher {
public:
    bool Initialize(const std::vector<GameConfig>& games);
    void Start();
    void Stop();
    ProcessInfo GetProcessInfo(const std::string& gameName) const;
    std::vector<ProcessInfo> GetActiveGames() const;
    using ProcessEventCallback = std::function<void(const std::string&, ProcessState, const ProcessInfo&)>;
    void Subscribe(ProcessEventCallback cb);

private:
    // 多策略检测协调器
    // 进程句柄缓存池
    // 检测线程
};

} // namespace optimizer::process
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│           ProcessWatcher                │
├─────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐      │
│  │ ETW 实时检测 │  │ 窗口枚举检测 │      │
│  │ (主)        │  │ (辅助验证)   │      │
│  └──────┬──────┘  └──────┬──────┘      │
│         │                │              │
│  ┌──────▼────────────────▼──────┐      │
│  │      状态机 (State Machine)   │      │
│  │  NotRunning → Starting →     │      │
│  │  Running → [Suspended] →     │      │
│  │  Exiting → NotRunning        │      │
│  └──────────────┬───────────────┘      │
│                 │                       │
│  ┌──────────────▼───────────────┐      │
│  │      句柄缓存池 (HandlePool)  │      │
│  │  - 自动权限降级               │      │
│  │  - 进程退出时自动关闭句柄     │      │
│  └──────────────────────────────┘      │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 句柄安全获取
```cpp
// 最小权限原则
HANDLE hProcess = OpenProcess(
    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION,
    FALSE, pid
);
// 绝不请求 PROCESS_ALL_ACCESS 或 VM_READ/WRITE
```

### 4.2 反作弊友好设计
- 不注入 DLL
- 不读写游戏进程内存
- 不挂起/恢复游戏线程
- 只通过公开 API 调整自身和系统参数
- 进程身份使用 PID + 创建时间/generation，不仅依赖 PID
- ETW 作为可选低延迟后端；第一版必须保留 Toolhelp 轮询兜底
- 服务模式下的窗口/前台检测放在交互用户 Agent，而不是 Session 0 服务

---

## 五、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| ConfigManager | 依赖 | 读取目标游戏列表 |
| MetricsCollector | 协作 | ETW 事件共享 |
| PolicyEngine | 被依赖 | 进程事件触发策略 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
