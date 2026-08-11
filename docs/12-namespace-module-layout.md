# 模块化开发规范：命名空间与文件夹映射

> **文档状态**：v1.0（2026-08-09）
> **目的**：把 docs 中的总体架构（02）落地为"命名空间 ↔ 文件夹一一对应"的模块化工程结构
> **适用范围**：本项目全部后续模块开发  
> **协作前置**：目录映射之外，任何 AI 和开发者还必须遵守 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md) 的引导式共同实现协议。

---

## 一、项目框架回顾（来自 docs/02-overall-architecture.md）

本项目是一个 Windows 游戏性能优化器，采用三层架构：

| 层 | 职责 | 包含模块 |
|----|------|---------|
| Layer 1 监控感知层 | 只读采集，零副作用 | MetricsCollector、ProcessWatcher、UserActivityDetector |
| Layer 2 持续维护层 | 低频、轻量、可中断 | **MemoryTuner（本轮）**、DiskCacheMaintainer、SchedulerTuner |
| Layer 3 应急响应层 | 激进但短暂，自动解除 | PowerLocker、GpuHeartbeat、PriorityBooster |
| 跨层基础设施 | 配置 / 策略 / 日志 / 宿主 | ConfigManager、PolicyEngine、Logger、ServiceHost |

详细设计见 [02-overall-architecture.md](02-overall-architecture.md) 及各模块设计文档（03~10）。

---

## 二、模块化开发规范

### 2.1 核心原则：命名空间 ↔ 文件夹一一对应

每个业务模块独占：

- 一个命名空间：`optimizer::<module>`（统一以项目名 `optimizer` 开头）
- 两个镜像目录：`include/<module>/`（头文件）、`source/<module>/`（实现）
  —— `include` 与 `source` 直接位于项目根目录下，目录层**不再包含 `optimizer`**

```
include/<module>/xxx.hpp   <->   namespace optimizer::<module>
source/<module>/xxx.cpp    <->   namespace optimizer::<module>
```

这样仅凭目录路径就能判断代码所属模块，也避免多个模块在全局命名空间互相污染。

### 2.2 全模块命名空间 / 目录映射表

| 模块 | 命名空间 | 头文件目录 | 源文件目录 | 设计文档 |
|------|---------|-----------|-----------|---------|
| ConfigManager | `optimizer::config` | `include/config` | `source/config` | 03 |
| MetricsCollector | `optimizer::metrics` | `include/metrics` | `source/metrics` | 04 |
| ProcessWatcher | `optimizer::process` | `include/process` | `source/process` | 05 |
| UserActivityDetector | `optimizer::activity` | `include/activity` | `source/activity` | 02 + 实现索引 14 |
| PolicyEngine | `optimizer::policy` | `include/policy` | `source/policy` | 06 |
| MemoryTuner | `optimizer::memory` | `include/memory` | `source/memory` | 07（Experimental） |
| DiskCacheMaintainer | `optimizer::disk` | `include/disk` | `source/disk` | 02 + 实现索引 14（Planned） |
| SchedulerTuner | `optimizer::scheduler` | `include/scheduler` | `source/scheduler` | 02 + 实现索引 14 |
| PowerLocker | `optimizer::power` | `include/power` | `source/power` | 08 |
| GpuHeartbeat | `optimizer::gpu` | `include/gpu` | `source/gpu` | 09（Experimental） |
| PriorityBooster | `optimizer::priority` | `include/priority` | `source/priority` | 02 + 实现索引 14 |
| ServiceHost | `optimizer::service` | `include/service` | `source/service` | 10 |
| Logger | `optimizer::logger` | `include/logger` | `source/logger` | 02 + 实现索引 14 |
| 公共设施（RAII/错误/编码等） | `optimizer::common` | `include/common` | `source/common` | 实现索引 14 |

### 2.3 文件命名规范

| 文件类型 | 命名规则 | 示例 |
|---------|---------|------|
| 模块头文件 | `模块名.hpp` | `memory_tuner.hpp` |
| 模块实现 | `模块名.cpp` | `memory_tuner.cpp` |
| 代码学习文档 | 与目标文件同名，扩展名 `.md` | `memory_tuner.md` |

> **本项目约定（引导式学习模式）**：真实 `.hpp/.cpp` 是实现真相；同名 `.md` 解释设计、API、边界、TODO、调试方法和验收。Markdown 不再以复制完整实现供机械手抄为默认方式。第一次出现的通用模式可以放入完整的小型安全示范；后续应按 L2～L4 逐渐改为骨架补全、契约实现和选型评审。

### 2.4 md 教学内容规范

对应目录下的 Markdown 至少应包含：

1. **教学级别与分工**：建议 L0～L4，AI 负责和学习者负责；
2. **API 卡片**：能力、选型原因、头文件、库、参数、成功/失败、错误域；
3. **资源配对**：拥有/借用/伪句柄，以及准确的释放函数；
4. **权限和副作用**：最小权限、支持条件和系统影响；
5. **绿色自由区**：学习者可自由改动和调试的内容；
6. **黄色不变量**：生命周期、锁、停止、身份和恢复约束；
7. **红色禁止区**：不可在日常环境直接启用的行为；
8. **实现步骤**：可独立编译或测试的小型 TODO，而不是几百行答案；
9. **失败注入与调试观察点**；
10. **验收和复盘问题**。

完整规范见 docs/00；每个 Win32/Nt API 首次进入项目时都应有卡片，而不是只在代码旁逐行翻译语法。

---

## 三、实现文档交付范围

各模块文档状态以 [14-implementation-document-index.md](14-implementation-document-index.md) 为准。经安全审查删除的旧“完整代码手抄”文档标记为待按 docs/25 重建；本轮不提供替代文件。其余模块文档保留接口契约、API 风险和验收标准。

### 3.0 文档状态标记

- **Baseline**：计划进入 MVP；
- **Experimental**：默认关闭，必须经 A/B 验证；
- **Planned**：仅保留边界和接口，不进入当前实施。

## 三之一、MemoryTuner 文档状态

旧 `include/memory/memory_tuner.md` 与 `source/memory/memory_tuner.md` 以完整代码手抄为核心，并包含不再允许直接使用的 Native 写示例，已在审查中移除。过早实现的 `.hpp/.cpp` 也因接口仍暴露禁用等级、重复公共错误设施和生命周期模型未定而移除。当前学习入口是 [07-module-memory-tuner.md](07-module-memory-tuner.md) 和 docs/20～24；未来按 docs/25 从只读契约重新建立。

### 3.1 当前涉及的 API 一览

| 类别 | API | 文档 |
|------|-----|------|
| 内存状态 | `GlobalMemoryStatusEx` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex) |
| 内存列表查询 | `NtQuerySystemInformation` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation) |
| 内存列表清理 | `NtSetSystemInformation` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/winternl/nf-winternl-ntsetsysteminformation) |
| 工作集收缩 | `EmptyWorkingSet` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/psapi/nf-psapi-emptyworkingset) |
| 工作集设置 | `SetProcessWorkingSetSize` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/memoryapi/nf-memoryapi-setprocessworkingsetsize) |
| 权限提升 | `OpenProcessToken` / `LookupPrivilegeValueW` / `AdjustTokenPrivileges` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges) |
| 同步 | `CreateEventW` / `SetEvent` / `WaitForSingleObject` / `CloseHandle` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/synchapi/) |
| 系统信息 | `GetSystemInfo` | [MS Learn](https://learn.microsoft.com/zh-cn/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsysteminfo) |

---

## 四、后续模块开发顺序建议

1. **Common + Logger + ConfigManager**：先解决资源、错误、日志和配置；
2. **MetricsCollector + ProcessWatcher**：建立只读观测；
3. **PolicyEngine**：先只输出审计决策，不执行动作；
4. **PowerLocker + PriorityBooster**：接入低风险、可回滚执行器；
5. **UserActivityDetector + ServiceHost/Agent**：处理交互会话与部署；
6. **MemoryTuner / SchedulerTuner / GpuHeartbeat**：仅作为默认关闭的实验模块；
7. **DiskCacheMaintainer**：在有明确 workload 和数据前保持 Planned。

每个模块开发时，先在本文件 2.2 节确认目录与命名空间，再按 docs/00 生成任务卡，并按 2.4 节完善对应 Markdown。AI 不得因为已有 `.md` 示例就跳过对真实源码和当前安全开关的检查。

---

*文档版本：v1.1 | 创建日期：2026-08-09 | 更新日期：2026-08-09 | 维护者：项目开发者*
