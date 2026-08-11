# 模块设计文档：性能监控采集器 (MetricsCollector)

> **所属层**：Layer 1 - 监控感知层  
> **模块ID**：MOD-MET-001  
> **状态**：大纲 + 首切片已落地（2026-08-11）  
> **学习协作建议**：新 PDH 模式从 L1 小型示范开始，随后以 L2 骨架补全。AI 提供 PDH/API 卡片、句柄生命周期和有效性模型；学习者实现聚合、单位转换、warming-up 处理和测试。绿色区是只读指标与聚合，黄色不变量是 PDH 错误域、停止/关闭顺序和无效样本不得冒充零值；红色区禁止借采集名义执行 Native 写。

> **已落地切片（2026-08-11，MET-001）**：`optimizer::metrics::MemorySample / MemoryWindowReport / AggregateMemoryWindow` 纯函数窗口聚合（整数、无浮点、溢出安全），以及 CLI `CppOptimizer.exe --observe <1..60>` 前台有界只读采样。详见 `include/metrics/memory_metrics.hpp` 头文件注释与 `source/metrics/memory_metrics.cpp` 实现注释。PDH/ETW、采集线程、MetricsBus 仍为大纲。

---

## 一、模块概述

### 1.1 职责
- 通过 PDH (Performance Data Helper) 采集系统性能计数器
- 可选通过 ETW (Event Tracing for Windows) 消费实时事件；第一版不将 ETW 作为硬依赖
- 指标聚合、降采样、异常检测
- 向 MetricsBus 发布结构化指标

### 1.2 采集指标清单

| 指标类别 | 具体指标 | 采集方式 | 频率 |
|---------|---------|---------|------|
| **CPU** | 整体使用率、每个核心使用率、中断率 | PDH | 100ms |
| **内存** | 提交字节、可用物理内存、Standby List 大小、Modified List 大小 | PDH + NtQuerySystemInformation | 100ms |
| **磁盘** | 每个逻辑盘的读写速率、队列深度、响应时间 | PDH | 500ms |
| **GPU** | 利用率、显存使用、温度、频率 | PDH (适配器特定计数器) | 500ms |
| **进程** | 目标进程的 CPU、内存、I/O、线程数 | PDH (Process 对象) | 100ms |
| **网络** | 带宽使用、延迟（可选） | PDH | 1000ms |
| **系统事件** | 进程创建/退出、电源状态变更、显示器状态 | ETW | 实时 |

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::metrics {

struct Metric {
    std::string name;
    std::string source;
    double value;
    uint64_t timestamp;
    std::map<std::string, std::string> labels;
};

class MetricsCollector {
public:
    bool Initialize();
    void Start();
    void Stop();
    bool AddCounter(const std::string& path, const std::string& alias);
    std::vector<Metric> GetLatestMetrics() const;
    using MetricCallback = std::function<void(const Metric&)>;
    void Subscribe(MetricCallback cb);

private:
    // PDH 查询句柄管理
    // ETW 会话管理
    // 采集线程
};

} // namespace optimizer::metrics
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│         MetricsCollector                │
├─────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    │
│  │  PDH 引擎    │    │  ETW 消费者  │    │
│  │ - 计数器注册 │    │ - 会话创建   │    │
│  │ - 批量查询   │    │ - 实时回调   │    │
│  │ - 双缓冲     │    │ - 事件过滤   │    │
│  └──────┬──────┘    └──────┬──────┘    │
│         │                   │           │
│  ┌──────▼───────────────────▼──────┐   │
│  │        指标聚合器 (Aggregator)    │   │
│  │  - 滑动窗口平均                   │   │
│  │  - 异常值过滤（3σ 原则）          │   │
│  │  - 降采样（100ms → 1s）           │   │
│  └──────────────┬──────────────────┘   │
│                 │                       │
│  ┌──────────────▼──────────────────┐   │
│  │        MetricsBus (发布)         │   │
│  │  线程安全队列 → 消费者分发        │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 PDH 计数器路径格式

- 优先使用 `PdhAddEnglishCounterW`，避免不同系统显示语言导致路径失效；
- 速率型计数器需要至少两次采样，第一次应标记为 WarmingUp；
- `Process(name)` 会出现 `name#1` 等实例，必须结合 `ID Process` 匹配 PID；
- GPU Engine 实例动态变化，需要刷新和按 PID/引擎聚合；温度、频率并非统一可用指标。

```
\\<Machine>\<Object>(<Instance>)\<Counter>
示例：
  \Processor(_Total)\% Processor Time
  \Process(YuanShen)\Working Set
  \GPU Engine(*)\Utilization Percentage
```

### 4.2 ETW 提供者订阅
- 系统提供者：`Microsoft-Windows-Kernel-Process` (进程事件)

### 4.3 性能优化
- PDH 查询使用 `PdhCollectQueryData` 批量采集；PDH 错误使用 `PDH_STATUS` 解释，不依赖 `GetLastError`
- 双缓冲：采集线程写 Buffer A，消费者读 Buffer B，定期交换
- 无锁队列：`moodycamel::ConcurrentQueue` 传递指标

---

## 五、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| ConfigManager | 依赖 | 读取采集频率、计数器配置 |
| Logger | 被依赖 | 记录采集异常 |
| PolicyEngine | 被依赖 | 消费指标做决策 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
