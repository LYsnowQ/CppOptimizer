# 模块设计文档：策略引擎 (PolicyEngine)

> **所属层**：跨层决策中枢  
> **模块ID**：MOD-POL-001  
> **状态**：大纲阶段  
> **学习协作建议**：默认 L3，由学习者主导规则、迟滞、冷却、冲突解析和确定性测试；AI 负责契约质询、危险动作门禁和审计字段。绿色区覆盖全部纯决策逻辑，黄色不变量是相同输入产生可解释且幂等的决策、无效指标不得触发动作；红色区是 PolicyEngine 直接调用 Win32 或绕过执行器/permit。

---

## 一、模块概述

### 1.1 职责
- 消费 MetricsBus 的实时指标流
- 根据配置规则和系统状态做出优化决策
- 生成优化指令并分发到各执行模块
- 防止优化冲突（如 Layer 2 和 Layer 3 同时操作同一资源）
- 维护期望状态与执行 ACK，不把“已分发”误认为“已成功”
- 为每条指令附带目标 generation、TTL、reason code，保证幂等和可审计

### 1.2 核心决策逻辑

```
输入：
  - 系统资源余量（内存/CPU/GPU/磁盘）
  - 目标游戏状态（未运行/前台/后台）
  - 用户活跃状态（活跃/AFK）
  - 当前已激活的优化措施

输出：
  - 优化指令集（启用/禁用/调整参数）
  - 目标模块（Layer 2 / Layer 3 具体执行器）
```

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::policy {

enum class OptimizationAction {
    EnableLayer3, DisableLayer3, TriggerMemoryTune,
    AdjustScheduler, SetGpuHeartbeat, NoOp
};

struct OptimizationCommand {
    OptimizationAction action;
    std::map<std::string, std::string> parameters;
    uint64_t validUntil;
    std::string reason;
};

class PolicyEngine {
public:
    bool Initialize();
    void Start();
    void Stop();
    void InjectDecision(OptimizationCommand cmd);
    std::vector<OptimizationCommand> GetActivePolicies() const;

private:
    // 规则评估引擎
    // 冲突解决器
    // 指令分发器
};

} // namespace optimizer::policy
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│           PolicyEngine                  │
├─────────────────────────────────────────┤
│  ┌─────────────────────────────────┐   │
│  │        规则评估器 (Rule Evaluator)│   │
│  │  IF 内存 > 95% THEN 休眠        │   │
│  │  IF 游戏前台 AND GPU < P0 THEN  │   │
│  │     启用 Layer 3                │   │
│  │  IF 游戏加载中 THEN 触发内存整理 │   │
│  └──────────────┬──────────────────┘   │
│                 │                       │
│  ┌──────────────▼──────────────────┐   │
│  │      冲突解决器 (Conflict Resolver)│   │
│  │  - 优先级：Layer 3 > Layer 2      │   │
│  │  - 互斥：电源锁定 vs 节能模式      │   │
│  │  - 防抖：状态切换冷却期 5s         │   │
│  └──────────────┬──────────────────┘   │
│                 │                       │
│  ┌──────────────▼──────────────────┐   │
│  │      指令分发器 (Dispatcher)      │   │
│  │  - Layer 3 → 紧急执行队列（高优） │   │
│  │  - Layer 2 → 维护任务队列（低优） │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 规则引擎设计
- 初期：纯 C++ 状态机/规则函数，保持确定性和可单元测试
- 进阶：DSL 规则表达式，支持热更新
- 考虑：`exprtk` 或自研简单表达式解析器

### 4.2 防抖机制
```cpp
class HysteresisFilter {
    bool state;
    std::chrono::steady_clock::time_point lastChange;
    std::chrono::milliseconds cooldown{5000};
public:
    bool Update(bool newState) {
        if (newState == state) return state;
        auto now = std::chrono::steady_clock::now();
        if (now - lastChange < cooldown) return state;
        state = newState;
        lastChange = now;
        return state;
    }
};
```

### 4.3 资源余量分级

| 级别 | 内存余量 | CPU 余量 | 策略 |
|------|---------|---------|------|
| Critical | < 5% | < 5% | 撤销实验动作，仅保留监控和必要恢复 |
| Tight | 5-15% | 5-15% | 只发软提示，不执行 |
| Adequate | 15-30% | 15-30% | 轻量维护，60s 间隔 |
| Comfortable | > 30% | > 30% | 完整优化策略 |

---

## 五、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| MetricsCollector | 依赖 | 消费指标 |
| ProcessWatcher | 依赖 | 游戏状态 |
| ConfigManager | 依赖 | 阈值配置 |
| 所有执行模块 | 被依赖 | 接收指令 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
