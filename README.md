# CppOptimizer

Windows 游戏运行场景的性能观测与受控优化学习项目。项目以正式软件工程方式开发，但**不承诺在所有设备或游戏上提高性能**；任何优化动作都必须可测量、可撤销、默认保守。

## 当前定位

- 主程序是 **x64 Windows 用户态程序**；
- 使用 Win32、PDH、DXGI，并少量研究 Native API；
- 当前不包含内核驱动，不注入、不 Hook、不读写游戏内存；
- MemoryTuner、GpuHeartbeat、SchedulerTuner 属于 Experimental，默认关闭；
- DiskCacheMaintainer 仅 Planned。

> 调用 `Nt*` API 不等于编写内核驱动。请先阅读 [用户态、Native API 与内核态边界](docs/20-user-native-kernel-boundary-learning-guide.md)。

## 设计原则

1. 安全和正确性优先于性能收益；
2. 先观测，再决策，最后执行；
3. 标准用户运行是默认路径；
4. 系统级动作使用多重门禁、租约、审计和恢复；
5. 没有配对 A/B 数据时，不默认开启优化；
6. 不以“可用内存增加”或“频率更高”单独证明游戏性能改善。

## 文档入口

| 文档 | 说明 |
|---|---|
| [总体架构](docs/02-overall-architecture.md) | 模块与数据流 |
| [文档审查与修正](docs/13-documentation-review-and-corrections.md) | 关键技术风险 |
| [实现文档索引](docs/14-implementation-document-index.md) | include/source 文档导航 |
| [用户态/Native/内核边界](docs/20-user-native-kernel-boundary-learning-guide.md) | 底层学习必读 |
| [正式工程手册](docs/21-formal-engineering-handbook.md) | 编码、构建和模块约束 |
| [错误、资源与并发](docs/22-error-resource-concurrency-guide.md) | Win32 正确性基础 |
| [危险操作与威胁模型](docs/23-dangerous-operation-policy-and-threat-model.md) | R0～R4 风险管理 |
| [测试与发布](docs/24-test-debug-release-playbook.md) | 实验、调试、发布流程 |
| [学习路线](docs/11-learning-roadmap.md) | 分阶段学习计划 |

## 构建环境

- Windows 10/11 x64；
- Visual Studio 2022，安装“使用 C++ 的桌面开发”和 Windows SDK；
- C++20；
- 当前以 MSBuild 工程为主，后续同步维护 CMake。

### Visual Studio

打开：

```text
CppOptimizer.slnx
```

选择：

```text
Debug | x64
```

第一阶段不建议使用 Win32，也不要以管理员身份启动 IDE。只有明确的单次权限实验才使用提升控制台。

### CMake（安装 CMake + Ninja 后）

```powershell
cmake --preset windows-x64-debug
cmake --build --preset build-debug
ctest --preset test-debug
.\out\build\windows-x64-debug\CppOptimizer.exe --diagnose
```

## 安全运行模式

已可用（全部只读）：

```text
CppOptimizer.exe --diagnose   平台与 Native API 能力探测
CppOptimizer.exe --status     单次只读内存快照（无轮询）
CppOptimizer.exe --observe <s> 每秒采样内存并输出窗口报告（1..60 秒，前台有界，只读）
```

计划提供：

```text
CppOptimizer.exe --console --config <path>
```

危险实验必须包含动作特定确认，不能仅使用通用 `--force`。示例：

```text
CppOptimizer.exe --experimental \
  --allow-native-memory-write \
  --memory-clean=light \
  --acknowledge-system-wide-side-effects
```

上述参数当前仅表示设计契约，不代表相关功能已经可用。

## 当前实施顺序

1. Common / Error / Platform；
2. Logger；
3. ConfigManager；
4. MetricsCollector + ProcessWatcher；
5. PolicyEngine 只读决策；
6. PowerLocker + PriorityBooster；
7. Agent/Service；
8. Experimental 模块。

## 重要安全边界

- 不要在保存重要数据的主机首次测试 Native 写操作；
- 不要使用 `REALTIME_PRIORITY_CLASS`；
- 不要默认清理 Standby/Modified List；
- 不要宣称 Power Request 能锁定 CPU/GPU P-State；
- 不要把 GPU 合成负载描述为零竞争；
- 不要加载来源不明的驱动或关闭系统安全机制；
- 不承诺所有反作弊均接受本程序，只能保证项目遵守公开行为边界。

## 贡献与安全

- 开发流程见 [CONTRIBUTING.md](CONTRIBUTING.md)；
- 安全报告见 [SECURITY.md](SECURITY.md)；
- 危险功能必须在 PR 中提供风险、门禁、回滚和测试证据。

## 项目状态

当前处于工程基线和基础设施建设阶段，不建议作为“系统优化工具”日常使用。

已完成的真实代码基线：统一错误模型、按释放函数区分的资源所有权、Native API 只读能力探测和只读诊断入口。MemoryTuner 已开始从 R0 只读契约重建，目前只提供一次物理内存状态查询和纯逻辑快照校验，不包含清理、线程、权限提升或 Native 写。MetricsCollector 已落地首个只读切片：`--observe` 前台有界观测窗口与整数窗口聚合（`optimizer::metrics`），PDH/ETW 与采集线程仍为大纲。
