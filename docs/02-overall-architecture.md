# Windows 游戏优化器 — 总体架构设计文档

> **文档状态**：设计基线（v0.2）  
> **目标读者**：项目开发者、技术评审  
> **核心原则**：以测量为先、默认保守、动作可回滚。任何“优化”都必须经过安全门禁和配对 A/B 验证；没有证据时保持关闭。  
> **重要修正**：详细风险和 API 语义见 [13-documentation-review-and-corrections.md](13-documentation-review-and-corrections.md)。开发者与任何 AI 在开始实现前还必须阅读 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md)，按任务卡、API 卡片、教学级别和绿/黄/红边界协作。

---

## 一、项目概述

### 1.1 项目背景
Windows 会根据电源策略、负载、温度和驱动策略动态调整资源状态。帧率波动和加载卡顿可能来自调度、着色器编译、存储、内存压力、驱动或游戏自身。本项目先建立只读观测闭环，再以最小、可撤销的系统动作做受控实验；任何合成负载都会造成实际资源竞争，不能宣称“零竞争”。

### 1.2 设计目标

| 目标 | 指标 | 验证方式 |
|------|------|---------|
| **低开销** | 空闲 CPU 均值 < 0.5%，工作集目标 < 30MB，句柄数稳定 | WPA/Performance Monitor 长时间采样 |
| **有效** | 帧时间 P95/P99 或 1% Low 在配对 A/B 中有可重复改善，且功耗/温度代价可接受 | PresentMon/CapFrameX，多轮同场景实验 |
| **安全** | 不修改系统文件、不注入、不 Hook、不读写游戏内存；所有副作用可回滚 | 代码审计 + 故障注入 + 行为监控 |
| **可配置** | 支持白名单、阈值、模块开关和实验功能显式 opt-in | 配置文件 + 命令行参数 |
| **可解释** | 每条动作记录 reason code、输入快照、执行结果和恢复结果 | 结构化日志审计 |

### 1.3 非目标
- ❌ 不替代显卡驱动优化
- ❌ 不做内存超频/硬件修改
- ❌ 不提供游戏内覆盖（Overlay）
- ❌ 不支持作弊/反作弊绕过

---

## 二、架构总览

### 2.1 三层架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Layer 3: 应急响应层                        │
│                  (Emergency Response Layer)                   │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  电源锁定器  │  │  GPU 心跳器  │  │  进程优先级提升器    │  │
│  │Power Locker │  │ GPU Heartbeat│  │ Priority Booster    │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│  触发条件：目标游戏进程启动 / 用户手动激活                      │
│  行为特征：激进但短暂，目标进程退出后自动解除                   │
├─────────────────────────────────────────────────────────────┤
│                    Layer 2: 持续维护层                        │
│                (Continuous Maintenance Layer)                │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  内存整理器  │  │  磁盘缓存维护 │  │  调度参数微调器      │  │
│  │Memory Tuner │  │ Disk Cache   │  │ Scheduler Tuner     │  │
│  │             │  │ Maintainer   │  │                     │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│  触发条件：定时器 / 系统空闲阈值满足 / 游戏加载间隙             │
│  行为特征：低频、轻量、可中断                                  │
├─────────────────────────────────────────────────────────────┤
│                    Layer 1: 监控感知层                        │
│              (Monitoring & Perception Layer)                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  性能计数器  │  │  进程生命周期 │  │  用户交互检测        │  │
│  │ 采集器      │  │  监控器      │  │  (AFK Detector)     │  │
│  │(PDH/ETW)    │  │(Process Watcher)│  │                     │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│  行为特征：只读、零副作用、毫秒级上报                          │
└─────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────┴─────────┐
                    │    配置管理中心     │
                    │  (Config Manager)  │
                    └───────────────────┘
```

### 2.2 数据流向

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   配置文件    │────▶│  ConfigMgr   │────▶│   各模块     │
│  (JSON/TOML) │     │   (单例)      │     │  (运行时参数) │
└──────────────┘     └──────────────┘     └──────────────┘
                              │
                              ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  性能计数器   │────▶│  MetricsBus  │────▶│  策略引擎    │
│  (原始数据)   │     │  (线程安全队列)│     │ (决策逻辑)   │
└──────────────┘     └──────────────┘     └──────┬───────┘
                                                  │
                    ┌─────────────────────────────┼─────────────┐
                    ▼                             ▼             ▼
              ┌─────────┐                  ┌─────────┐    ┌─────────┐
              │ Layer 3 │                  │ Layer 2 │    │ 日志/上报│
              │ 执行器  │                  │ 执行器  │    │         │
              └─────────┘                  └─────────┘    └─────────┘
```

---

## 三、模块职责矩阵

| 模块 | 所属层 | 核心职责 | 输入 | 输出 | 关键指标 |
|------|--------|---------|------|------|---------|
| **ConfigManager** | 跨层 | 配置解析、热重载、校验 | 配置文件/命令行 | 配置对象 | 加载时间 < 50ms |
| **MetricsCollector** | Layer 1 | PDH/ETW 数据采集 | 系统计数器路径 | 结构化指标 | 采样间隔 100ms |
| **ProcessWatcher** | Layer 1 | 目标进程发现与生命周期跟踪 | 进程名/窗口标题 | 进程事件(启动/退出) | 检测延迟 < 500ms |
| **UserActivityDetector** | Layer 1 | 键盘/鼠标/全屏状态检测 | Raw Input / WinEvents | 用户活跃状态 | 无额外 CPU 开销 |
| **PolicyEngine** | 跨层 | 基于指标和配置的决策 | 指标流 + 配置 | 优化指令 | 决策延迟 < 10ms |
| **MemoryTuner** | Layer 2 | Standby List 清理、工作集调整 | 内存压力指标 | 清理结果 | 单次操作 < 100ms |
| **DiskCacheMaintainer** | Layer 2 | 预读策略、缓存热度维护 | 磁盘 I/O 指标 | I/O 优化提示 | 无阻塞 I/O |
| **SchedulerTuner** | Layer 2 | 线程/I/O 优先级微调 | 调度指标 | 优先级调整指令 | 只影响后台进程 |
| **PowerLocker** | Layer 3 | 电源请求创建、高性能态锁定 | 游戏启动事件 | 电源请求句柄 | 自动释放保证 |
| **GpuHeartbeat** | Layer 3 | 隐藏 D3D 设备、GPU 负载维持 | GPU 状态指标 | 渲染心跳 | 负载 < 3% |
| **PriorityBooster** | Layer 3 | 目标进程优先级提升 | 进程句柄 + 配置 | 优先级恢复令牌 | 可逆操作 |
| **Logger** | 跨层 | 结构化日志、日志轮转、级别控制 | 各模块日志事件 | 日志文件 | 异步写入 |
| **ServiceHost** | 跨层 | Windows 服务封装、生命周期管理 | SCM 控制请求 | 服务状态 | 优雅启停 |

---

## 四、关键技术决策

### 4.1 开发环境选择

| 维度 | 选择 | 理由 |
|------|------|------|
| **操作系统** | Windows 10/11 | 目标平台，WSL2 可用于交叉编译验证 |
| **编译器** | MSVC (Visual Studio 2022) | 最佳 Windows SDK 支持，调试体验好 |
| **构建系统** | CMake + Ninja | 跨平台潜力、现代 C++ 生态、CLion/VS 都支持 |
| **编辑器** | VS Code + Clangd | 轻量、扩展丰富、远程开发友好（推荐） |
| **C++ 标准** | C++20 | `std::format`, `concepts`, `coroutines`（潜在） |
| **包管理** | vcpkg | Windows 原生包管理，与 CMake 集成好 |

### 4.2 架构模式

| 模式 | 应用场景 | 实现方式 |
|------|---------|---------|
| **Reactor** | 事件驱动（进程监控、用户输入） | 基于 `WaitForMultipleObjects` + 事件队列 |
| **生产者-消费者** | 指标采集 → 策略决策 | 无锁队列 (`moodycamel::ConcurrentQueue`) |
| **RAII** | 资源管理（句柄、令牌、请求） | 自定义 `AutoHandle`, `AutoPowerRequest` |
| **策略模式** | 不同游戏的不同优化策略 | 策略接口 + 配置映射 |
| **观察者模式** | 模块间事件通知 | `std::function` 回调 + 事件总线 |

### 4.3 线程模型

```
主线程 (Main Thread)
├── 配置加载与热重载
├── 服务控制分发 (SCM)
└── 优雅退出协调

监控线程 (Monitor Thread) - 1个
├── PDH 查询定时采集
├── ETW 事件消费
└── 指标聚合与上报

工作线程池 (Worker Pool) - N个 (N = CPU核心数/2)
├── Layer 2 维护任务
├── Layer 3 应急响应
└── 日志异步写入

专用线程 (Dedicated Threads)
├── GPU 心跳线程 (1个，绑定到特定核心)
├── 进程轮询线程 (1个，1秒间隔)
└── 用户输入监听线程 (1个，Raw Input)
```

### 4.4 安全与权限模型

```
┌─────────────────────────────────────────┐
│           用户态 (User Mode)             │
│  ┌─────────────────────────────────┐   │
│  │  标准用户权限运行（默认）          │   │
│  │  - Layer 1 全部功能               │   │
│  │  - Layer 2 大部分功能             │   │
│  │  - Layer 3 有限功能               │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │  管理员权限运行（可选安装）        │   │
│  │  - 完整 Layer 3 功能              │   │
│  │  - 系统级内存操作                 │   │
│  │  - 其他进程优先级调整             │   │
│  └─────────────────────────────────┘   │
├─────────────────────────────────────────┤
│           内核态 (Kernel Mode)           │
│  ┌─────────────────────────────────┐   │
│  │  可选驱动组件（进阶，Phase 2）     │   │
│  │  - MiniFilter I/O 拦截            │   │
│  │  - 内核回调注册                   │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## 五、部署形态

### 5.1 三种运行模式

| 模式 | 形态 | 适用场景 | 权限要求 |
|------|------|---------|---------|
| **独立应用** | 普通 EXE，托盘图标 | 个人用户，临时使用 | 标准用户 |
| **Windows 服务** | 后台服务，无界面 | 长期运行，开机自启 | 管理员（安装时） |
| **用户代理 + 可选服务** | Per-user Agent 负责窗口/输入，Service 负责受控高权限动作 | 长期运行、跨 Session 正确协作 | 安装服务时需管理员 |

### 5.2 配置体系

```yaml
# config.yaml 结构（大纲）
version: "1.0"

# 目标游戏白名单
games:
  - name: "Genshin Impact"
    process: ["YuanShen.exe", "GenshinImpact.exe"]
    window_title: "原神"
    strategy: "aggressive"
  - name: "Black Myth: Wukong"
    process: ["b1.exe"]
    strategy: "balanced"

# 系统阈值
thresholds:
  critical_memory_percent: 95   # >95% 进入休眠
  tight_memory_percent: 85      # 85-95% 只发软提示
  comfortable_memory_percent: 70 # <70% 正常介入

# 各层开关
layers:
  layer1_monitoring: true
  layer2_maintenance: true
  layer3_emergency: true

# 日志
logging:
  level: "info"
  path: "%LOCALAPPDATA%\GameOptimizer\logs"
  max_size_mb: 100
  max_files: 5
```

---

## 六、里程碑规划

| 阶段 | 目标 | 时间 | 交付物 |
|------|------|------|--------|
| **MVP** | Layer 1 + 基础 Layer 2 | 4 周 | 可运行的内存清理工具 |
| **Alpha** | 完整三层架构 | +4 周 | 带 GUI 配置界面的完整版本 |
| **Beta** | 服务化 + 多游戏支持 | +4 周 | Windows 服务形态，支持 10+ 游戏 |
| **Release** | 性能验证 + 文档完善 | +4 周 | 开源发布，博客文章，面试弹药 |

---

## 七、风险与应对

| 风险 | 可能性 | 影响 | 应对策略 |
|------|--------|------|---------|
| 反作弊误报 | 高 | 致命 | 不注入、不 Hook、不读写游戏内存 |
| Windows 更新破坏兼容性 | 中 | 高 | 抽象层封装，单元测试覆盖 |
| 用户设备"刚刚好"导致负优化 | 中 | 中 | 自适应阈值、A/B 测试框架 |
| 未文档化 API 行为变更 | 低 | 高 | 优先使用文档化 API，Nt 系列加版本检查 |

---

## 八、相关文档索引

| 文档 | 说明 |
|------|------|
| [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md) | 项目级 AI 协作、教学梯度、自由调试边界和跨会话接续协议 |
| [01-help-documentation-guide.md](01-help-documentation-guide.md) | API 文档查阅指南 |
| [03-module-config-manager.md](03-module-config-manager.md) | 配置管理中心详细设计 |
| [04-module-metrics-collector.md](04-module-metrics-collector.md) | 性能监控模块详细设计 |
| [05-module-process-watcher.md](05-module-process-watcher.md) | 进程监控模块详细设计 |
| [06-module-policy-engine.md](06-module-policy-engine.md) | 策略引擎详细设计 |
| [07-module-memory-tuner.md](07-module-memory-tuner.md) | 内存优化模块详细设计 |
| [08-module-power-locker.md](08-module-power-locker.md) | 电源管理模块详细设计 |
| [09-module-gpu-heartbeat.md](09-module-gpu-heartbeat.md) | GPU 心跳模块详细设计 |
| [10-module-service-host.md](10-module-service-host.md) | 服务宿主模块详细设计 |
| [11-learning-roadmap.md](11-learning-roadmap.md) | 学习路线图 |
| [12-namespace-module-layout.md](12-namespace-module-layout.md) | 命名空间、目录与学习文档规范 |
| [13-documentation-review-and-corrections.md](13-documentation-review-and-corrections.md) | 风险修正、工程约束与测试门禁 |
| [14-implementation-document-index.md](14-implementation-document-index.md) | 全部 include/source 实现文档索引 |
| [15-module-logger.md](15-module-logger.md) | 结构化日志器详细设计 |
| [16-module-user-activity-detector.md](16-module-user-activity-detector.md) | 用户活动检测器详细设计 |
| [17-module-priority-booster.md](17-module-priority-booster.md) | 优先级提升器详细设计 |
| [18-module-scheduler-tuner.md](18-module-scheduler-tuner.md) | 调度微调器详细设计 |
| [19-module-disk-cache-maintainer.md](19-module-disk-cache-maintainer.md) | 磁盘缓存模块边界与暂缓条件 |
| [20-user-native-kernel-boundary-learning-guide.md](20-user-native-kernel-boundary-learning-guide.md) | 用户态、Native API 与真正内核态的边界 |
| [21-formal-engineering-handbook.md](21-formal-engineering-handbook.md) | 正式编码、构建、依赖与交付规范 |
| [22-error-resource-concurrency-guide.md](22-error-resource-concurrency-guide.md) | Windows 错误域、资源和并发详解 |
| [23-dangerous-operation-policy-and-threat-model.md](23-dangerous-operation-policy-and-threat-model.md) | 风险分级、许可、安全模式与威胁模型 |
| [24-test-debug-release-playbook.md](24-test-debug-release-playbook.md) | 测试、实验、调试和发布流程 |
| [25-module-document-template.md](25-module-document-template.md) | 新模块正式文档与学习协作模板 |
| [26-ai-session-task-record-template.md](26-ai-session-task-record-template.md) | 跨设备、跨 AI 的会话与任务记录模板 |

---

*文档版本：v0.2 | 创建日期：2026-08-08 | 更新日期：2026-08-09 | 状态：设计基线*
