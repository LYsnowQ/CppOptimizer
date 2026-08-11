# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added

- 正式工程、错误/资源/并发、安全和测试规范；
- 用户态、Native API 与内核态边界学习指南；
- 全模块 include/source 实现文档索引；
- README、SECURITY、CONTRIBUTING 和安全默认配置示例；
- 统一 `Result<T>/Error`、Win32/SCM 资源封装和 Native API 能力探测；
- `--diagnose` 只读入口与 Common 最小测试；
- CMake/CMakePresets 安全构建基线；
- MemoryTuner：`FormatBytes`（二进制单位显示、整数取整）与 `IsSnapshotFresh`（快照时效）纯函数及 7 个单元测试；
- `--status` 只读内存快照命令（单次查询，无轮询）；
- MetricsCollector 首切片：`AggregateMemoryWindow` 整数窗口聚合（min/avg/max 负载、min/max 可用内存，无浮点、溢出安全）及 7 个单元测试（`CppOptimizerMetricsTests`）；
- `--observe <seconds>` 前台有界只读采样命令（1..60 秒，无后台线程）；
- MEM-003 归档与 AI 审查修正（`maxAge == 0` 边界契约恢复 + 零窗口测试）。

### Changed

- work.md 第 3 版：确立“项目主线优先”工作流（每轮先推进项目增量，学习从增量派生）；原 MEM-004“可注入 backend”设计不推进项目，已取消并替换为 MET-001 指标切片；
- docs/00 增加“项目主线原则”（1.1），第 14 节改为“项目推进顺序”；
- docs/04 标记 MetricsCollector 首切片已落地。

### Changed

- MemoryTuner、GpuHeartbeat、SchedulerTuner 明确为 Experimental；
- 自动内存清理和系统级动作默认关闭；
- 项目优先发布 x64 用户态版本，驱动开发推迟到独立评审阶段；
- 移除过早的 MemoryTuner 实现与旧完整手抄文档，未来从只读契约和安全门禁重新建立；
- 清理 Visual Studio 本机缓存、用户设置和空依赖目录；
- 从 R0 只读契约开始重建 MemoryTuner：加入物理内存快照、输入校验和独立测试目标，不恢复任何清理能力。
