# 各目录实现文档索引与开发顺序

> **状态**：v1.1（2026-08-09）  
> **协作入口**：实现任何模块前先阅读 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md)。模块优先级描述工程顺序；教学级别 L0～L4 和 AI/学习者分工需按当前能力另行声明。

## 1. 项目级协作与模板

| 文档 | 用途 |
|---|---|
| [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md) | AI/学习者分工、L0～L4、API 卡片、绿/黄/红边界和跨会话规则 |
| [25-module-document-template.md](25-module-document-template.md) | 新模块的正式设计与学习协作模板 |
| [26-ai-session-task-record-template.md](26-ai-session-task-record-template.md) | 需要长期保留时记录单次任务、验证和学习证据 |

## 2. 模块命名空间与优先级

> 2026-08-11：`include/`、`source/` 下的模块 md 文档已全部移除，不再单独维护；模块契约以头文件注释与真实代码为真相。

| 模块 | 命名空间 | 优先级 |
|---|---|---|
| Common | `optimizer::common` | P0 |
| ConfigManager | `optimizer::config` | P0 |
| Logger | `optimizer::logger` | P0 |
| MetricsCollector | `optimizer::metrics` | P1 |
| ProcessWatcher | `optimizer::process` | P1 |
| UserActivityDetector | `optimizer::activity` | P2 |
| PolicyEngine | `optimizer::policy` | P1 |
| MemoryTuner | `optimizer::memory` | P2/R0 Baseline；清理能力仍为 Experimental |
| DiskCacheMaintainer | `optimizer::disk` | P3 |
| SchedulerTuner | `optimizer::scheduler` | P2 |
| PowerLocker | `optimizer::power` | P1 |
| GpuHeartbeat | `optimizer::gpu` | P3/Experimental |
| PriorityBooster | `optimizer::priority` | P1 |
| ServiceHost | `optimizer::service` | P2 |

## 3. 推荐实施批次

1. **基础设施**：Common → Logger → ConfigManager；
2. **只读闭环**：MetricsCollector + ProcessWatcher → PolicyEngine（只记录决策）；
3. **低风险执行**：PowerLocker + PriorityBooster；
4. **运行形态**：控制台宿主 → ServiceHost / Per-user Agent；
5. **实验模块**：MemoryTuner、SchedulerTuner、GpuHeartbeat；
6. **暂缓模块**：DiskCacheMaintainer，直到形成明确且可测的需求。

## 4. 公共基础层补充

正式代码新增统一基础层：

- `include/common/error.hpp` / `source/common/error.cpp`：错误域与 `Result<T>`；
- `include/common/unique_resource.hpp`：按释放函数区分的资源所有权；
- `include/platform/native_api.hpp` / `source/platform/native_api.cpp`：Native API 运行时能力探测和隔离边界；

未文档化 API 禁止绕过 platform 层直接进入新业务模块。

## 5. 模块实现必须回答的问题（原“source 文档要求”，模块 md 已移除，清单保留）

- 本模块建议使用哪个教学级别，AI 与学习者分别完成什么？
- 哪些属于绿色自由区、黄色不变量和红色禁止区？
- 新 API 的卡片是否覆盖失败值、错误域、最小权限、所有权和释放函数？
- 输入、输出、线程和所有权是什么？
- 使用哪些 Win32 API，头文件、库、成功条件和资源配对是什么？
- 标准用户和管理员下分别如何降级？
- 如何停止、取消、超时和恢复？
- 如何避免在回调持锁时重入？
- 单元测试、集成测试和人工验收步骤是什么？
- 哪些功能默认关闭，为什么？

## 6. Definition of Done

模块只有满足以下条件才视为完成：接口文档、实现文档、真实 `.hpp/.cpp`、编译通过、错误路径测试、资源泄漏检查、日志字段、最小 demo、项目文件/CMake 更新全部完成。学习阶段还必须记录至少一个由学习者独立完成的函数、测试或安全变体，以及能够说明的关键失败路径；这不替代正式质量门禁。
