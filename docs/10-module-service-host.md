# 模块设计文档：服务宿主 (ServiceHost)

> **所属层**：跨层基础设施  
> **模块ID**：MOD-SVC-001  
> **状态**：大纲阶段  
> **学习协作建议**：控制台宿主可用 L2，SCM、Session 0 和 ACL 从 L0/L1 共同实现。AI 主导服务状态机、安全描述符、恢复和 IPC 威胁边界；学习者实现控制台模式、状态映射和 fake SCM 测试。绿色区是控制台生命周期，黄色不变量是 SCM 超时、幂等停止和受保护 IPC；红色区是 SYSTEM 服务接受任意用户命令或直接采集交互会话输入。

---

## 一、模块概述

### 1.1 职责
- 封装 Windows 服务控制管理器（SCM）交互
- 处理服务安装、卸载、启动、停止
- 服务状态上报（Running/Paused/Stopped）
- 控制台模式兼容（调试时以普通 EXE 运行）
- 协调 Session 0 服务与交互用户 Agent；服务不直接负责窗口/输入检测

### 1.2 运行模式

| 模式 | 入口点 | 用途 |
|------|--------|------|
| **服务模式** | `StartServiceCtrlDispatcher` | 生产环境，后台运行 |
| **控制台模式** | `wmain` | 开发调试，直接运行 |
| **安装模式** | `CreateService` | 首次部署，注册服务 |

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::service {

enum class RunMode {
    Console,    // 控制台模式
    Service,    // Windows 服务模式
    Install,    // 安装服务
    Uninstall   // 卸载服务
};

class ServiceHost {
public:
    bool Initialize(RunMode mode);
    int Run();
    void RequestStop();

    // 服务控制回调
    using ControlHandler = std::function<void(DWORD control)>;
    void SetControlHandler(ControlHandler handler);

private:
    // SCM 交互
    // 服务主函数
    // 控制处理函数
};

} // namespace optimizer::service
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│           ServiceHost                   │
├─────────────────────────────────────────┤
│  ┌─────────────────────────────────┐   │
│  │      模式路由器 (Mode Router)     │   │
│  │  argc/argv 解析 → 确定运行模式    │   │
│  │  --console / --service /          │   │
│  │  --install / --uninstall          │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │      服务控制分发器               │   │
│  │  StartServiceCtrlDispatcher()    │   │
│  │  → ServiceMain()                 │   │
│  │  → RegisterServiceCtrlHandlerEx()│   │
│  │  → HandlerEx(SERVICE_CONTROL_*)  │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │      生命周期协调器               │   │
│  │  - 初始化所有模块                 │   │
│  │  - 主事件循环 (WaitForSingleObject)│  │
│  │  - 优雅停止：反初始化所有模块      │   │
│  │  - 设置服务状态为 STOPPED         │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 服务入口点
```cpp
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    // 注册控制处理器
    g_statusHandle = RegisterServiceCtrlHandlerEx(
        SERVICE_NAME, HandlerEx, nullptr
    );

    // 报告正在启动
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // 初始化业务逻辑
    // ...

    // 报告正在运行
    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // 主事件循环
    WaitForSingleObject(g_stopEvent, INFINITE);

    // 报告正在停止
    ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);

    // 清理
    // ...

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}
```

### 4.2 控制台模式兼容
```cpp
// 同一套代码，两种入口
int wmain(int argc, wchar_t* argv[]) {
    if (ParseArgs(argc, argv) == RunMode::Console) {
        // 设置控制台控制处理
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        // 直接运行业务逻辑
        return RunBusinessLogic();
    }
    // 服务模式
    SERVICE_TABLE_ENTRY entries[] = {
        { SERVICE_NAME, ServiceMain },
        { nullptr, nullptr }
    };
    StartServiceCtrlDispatcher(entries);
}
```

### 4.3 服务安装/卸载
```cpp
// 安装
SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
SC_HANDLE service = CreateService(
    scm, SERVICE_NAME, DISPLAY_NAME,
    SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
    SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
    exePath, nullptr, nullptr, nullptr, nullptr, nullptr
);

// 卸载
SC_HANDLE service = OpenService(scm, SERVICE_NAME, DELETE);
DeleteService(service);
```

---

## 五、Session 0 与 Agent 边界

Windows 服务运行在 Session 0，不能可靠地直接枚举交互用户窗口、判断前台窗口或获取用户最后输入。生产形态应拆分为 Service + Per-user Agent，并通过带 ACL、版本和长度限制的命名管道通信。MVP 可先只提供控制台/托盘模式。

---

## 六、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| 所有业务模块 | 依赖 | 初始化和反初始化 |
| Logger | 被依赖 | 记录服务状态变更 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
