# 学习路线图：Windows 系统编程与 C++ 性能工程

> **目标读者**：准大四学生，有 Reactor 项目经验、Git 和 Docker 使用经验  
> **时间跨度**：16 周（约 4 个月，覆盖秋招关键期）  
> **最终产出**：可演示的 Windows 游戏优化器 + 技术博客 + 面试弹药库  
> **学习协作基线**：全程采用 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md) 的引导式共同实现。AI 不让初学者盲猜 API，也不包办全部实现；通过补全、调试、变体和复盘逐步从 L0/L1 提升到 L3/L4。

---

## 一、前置评估：你的起点

### 已具备的能力（可直接利用）
| 技能 | 本项目中的价值 | 迁移建议 |
|------|-------------|---------|
| **Reactor 模式经验** | 事件驱动架构的核心，本项目 Layer 1 大量使用 | 将 Linux epoll 思维映射到 Windows `WaitForMultipleObjects` |
| **Git 经验** | 版本控制、分支管理、协作流程 | 直接沿用，建议学习 GitHub Actions 做 CI |
| **Docker 经验** | 容器化思维、环境隔离 | Windows 开发用不到，但可用于 Linux 交叉编译验证 |
| **Vim/VS Code** | 编辑器熟练度 | **推荐 VS Code + Clangd + CMake Tools**，Windows 开发体验最佳 |

### 需要补充的知识缺口
| 领域 | 缺口程度 | 优先级 |
|------|---------|--------|
| Windows API / Win32 编程 | 🔴 大 | P0 |
| C++ 现代特性 (17/20) | 🟡 中 | P1 |
| Windows 调试工具链 | 🟡 中 | P1 |
| CMake 构建系统 | 🟡 中 | P1 |
| 性能分析方法论 | 🟡 中 | P2 |
| Native API 风险与系统调用边界 | 🟡 中 | P1 |
| 驱动开发 / WDK | 🔴 大（独立进阶项目，当前不实施） | P4 |

---

## 二、阶段规划（16 周）

### Phase 1：Windows 基础筑基（第 1-4 周）
**目标**：能独立写出编译运行的 Win32 程序，理解核心概念

#### Week 1：开发环境搭建 + Hello Windows
- [ ] 安装 Visual Studio 2022 Community（勾选"使用 C++ 的桌面开发" + "Windows 11 SDK"）
- [ ] 配置 VS Code + Clangd + CMake Tools（推荐作为主要编辑器）
- [ ] 安装 vcpkg 并集成到 CMake
- [ ] 写出第一个 Win32 程序：`MessageBox` + 窗口消息循环
- [ ] 阅读：[Microsoft Learn - Windows 程序入门](https://learn.microsoft.com/zh-cn/windows/win32/learnwin32/learn-to-program-for-windows)

#### Week 2：进程与线程
- [ ] 掌握 `CreateProcess`、最小权限 `OpenProcess` 和等待自建子进程退出；`TerminateProcess` 只学习语义与风险，不对任意进程练习
- [ ] 掌握 `CreateThread`, `WaitForSingleObject`, `WaitForMultipleObjects`
- [ ] 理解进程句柄、线程句柄的生命周期（RAII 封装练习）
- [ ] 练习：写一个进程枚举器（`CreateToolhelp32Snapshot`）
- [ ] 阅读：[MS Learn - 进程和线程](https://learn.microsoft.com/zh-cn/windows/win32/procthread/processes-and-threads)

#### Week 3：内存管理基础
- [ ] 理解 Windows 虚拟内存模型（VAD、工作集、页文件）
- [ ] 掌握 `VirtualAlloc`, `VirtualFree`, `GlobalMemoryStatusEx`
- [ ] 理解 Standby List / Modified List / Zero List 概念
- [ ] 练习：写一个内存状态监控工具（每秒打印内存使用情况）
- [ ] 阅读：[Windows Internals 第 7 版 Part 1 - 第 5 章内存管理](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/)

#### Week 4：同步与 IPC
- [ ] 掌握事件（Event）、互斥量（Mutex）、信号量（Semaphore）
- [ ] 理解 `WaitForMultipleObjects` 的 Reactor 模式映射
- [ ] 练习：用事件实现一个简单的生产者-消费者队列
- [ ] 对比：Windows 同步原语 vs Linux pthread / futex
- [ ] **里程碑**：提交第一个独立 Win32 工具到 GitHub

**Phase 1 产出**：
- GitHub 仓库初始化
- 3-4 个独立小工具（进程枚举器、内存监控器、事件队列）
- 一篇博客：《从 Linux 到 Windows：一个 Reactor 程序员的系统编程入门》

---

### Phase 2：核心能力构建（第 5-8 周）
**目标**：掌握本项目核心 API，能写出模块原型

#### Week 5：PDH 性能计数器
- [ ] 理解 PDH 架构（查询 → 计数器 → 数据）
- [ ] 掌握 `PdhOpenQuery`, `PdhAddCounter`, `PdhCollectQueryData`
- [ ] 练习：写一个 CPU/内存/磁盘实时监控器（控制台输出）
- [ ] 阅读：[MS Learn - 使用 PDH API 消费计数器数据](https://learn.microsoft.com/en-us/windows/win32/perfctrs/consuming-counter-data)

#### Week 6：电源管理与进程优先级
- [ ] 掌握 `PowerCreateRequest`, `PowerSetRequest`, `SetThreadExecutionState`
- [ ] 掌握 `SetPriorityClass`, `SetThreadPriority`, I/O 优先级
- [ ] 理解 Windows 电源计划 GUID 和切换机制
- [ ] 练习：写一个“防睡眠小工具” + 仅调整自建测试子进程的优先级租约；默认最高 AboveNormal，并验证恢复
- [ ] 阅读：[MS Learn - 电源管理](https://learn.microsoft.com/en-us/windows/win32/power/power-management-portal)

#### Week 7：Native API 边界与安全实验
- [ ] 先阅读 [20-user-native-kernel-boundary-learning-guide.md](20-user-native-kernel-boundary-learning-guide.md)，理解调用 `Nt*` 仍属于 Ring 3
- [ ] 学习 NTSTATUS、系统调用边界、运行时符号解析和能力探测
- [ ] 先实现只读 `NtQuerySystemInformation` 适配层，不直接暴露未文档化结构
- [ ] 理解 Standby List 是可回收缓存，不能以“清空”为默认优化
- [ ] 在 VM/专用测试机、编译与运行双重开关下，才做单次 Light purge 实验
- [ ] 禁止自动 `FlushModifiedList`，禁止在日常主机首次测试
- [ ] 阅读：docs/22～24 的错误、危险操作和测试流程

#### Week 8：ETW 事件追踪
- [ ] 理解 ETW 架构（控制器 → 提供者 → 消费者）
- [ ] 掌握 `StartTrace`, `EnableTraceEx2`, `ProcessTrace`
- [ ] 练习：订阅进程创建/退出事件，实时打印进程信息
- [ ] 对比：ETW vs Linux eBPF / perf
- [ ] **里程碑**：MemoryTuner + ProcessWatcher 原型完成

**Phase 2 产出**：
- MemoryTuner 只读状态原型；Native 写实验保持默认关闭
- ProcessWatcher 可运行原型（检测游戏进程）
- 一篇博客：《用 PDH 和 ETW 打造 Windows 性能监控工具》

---

### Phase 3：架构整合（第 9-12 周）
**目标**：将各模块整合为完整三层架构，实现核心优化逻辑

#### Week 9：现代 C++ 工程化
- [ ] 学习 C++20 关键特性：`std::format`, `concepts`, `ranges`（基础）
- [ ] 掌握 CMake 现代用法：target-based, presets, install
- [ ] 引入第三方库：
  - `toml++`（配置解析）
  - `spdlog`（日志）
  - `moodycamel::ConcurrentQueue`（无锁队列）
- [ ] 重构 Phase 2 代码：RAII 句柄封装、异常安全、现代 C++ 风格

#### Week 10：模块整合与事件总线
- [ ] 设计 MetricsBus（线程安全队列 + 发布订阅）
- [ ] 实现 ConfigManager（TOML 解析 + 热重载骨架）
- [ ] 整合：MetricsCollector → MetricsBus → PolicyEngine
- [ ] 练习：实现一个简单的规则引擎（if-else 版本）

#### Week 11：Layer 3 应急响应
- [ ] 实现 PowerLocker（只做 Power Request；电源计划切换保持默认关闭）
- [ ] 为 GpuHeartbeat 先实现 Probe/门禁/停止框架；合成负载保持默认关闭
- [ ] 实现 PriorityBooster（进程优先级提升与恢复）
- [ ] 整合：PolicyEngine → Layer 3 执行器

#### Week 12：服务化与部署
- [ ] 实现 ServiceHost（SCM 交互 + 控制台兼容）
- [ ] 实现安装/卸载脚本
- [ ] 配置日志系统（结构化日志、日志轮转）
- [ ] **里程碑**：只读闭环 + R1 可逆动作可运行，至少 3 个测试进程规则；不以游戏数量代替正确性

**Phase 3 产出**：
- 完整可运行的优化器（EXE + 服务两种模式）
- 配置文件体系
- 一篇博客：《从零构建 Windows 系统级游戏优化器：架构设计与实现》

---

### Phase 4：验证与打磨（第 13-16 周）
**目标**：量化验证效果，完善文档，准备面试

#### Week 13：性能验证
- [ ] 搭建测试环境：
  - 测试游戏：原神、黑神话悟空、CS2
  - 监控工具：CapFrameX / MSI Afterburner / PresentMon
  - 对比实验：开优化器 vs 关优化器（各 30 分钟）
- [ ] 收集数据：帧率、帧时间、1% Low、加载时间
- [ ] 分析：统计显著性检验（t-test）

#### Week 14：边界条件测试
- [ ] 低内存设备测试（8GB 物理内存）
- [ ] 多游戏同时运行测试
- [ ] 长时间运行稳定性测试（24 小时）
- [ ] 反兼容性测试：确认不触发 EAC/BattlEye 误报
- [ ] 修复 Bug，优化性能

#### Week 15：文档与开源准备
- [ ] 完善 README（功能说明、编译指南、使用教程）
- [ ] 完善架构文档（补充实现细节到各模块设计文档）
- [ ] 编写 CHANGELOG
- [ ] 开源发布到 GitHub（MIT 许可证）
- [ ] 撰写系列博客（3-5 篇）

#### Week 16：面试准备
- [ ] 整理项目亮点（用 STAR 法则）
- [ ] 准备技术深挖问题：
  - "为什么用 Nt API 而不是文档化 API？"
  - "如何确保反作弊不拦截你的程序？"
  - "如果 Windows 更新导致 API 行为变更怎么办？"
  - "你的优化器在 8GB 内存设备上会不会负优化？"
- [ ] 模拟面试（找同学或 AI 对练）
- [ ] **里程碑**：项目开源 + 博客发布 + 面试准备就绪

**Phase 4 产出**：
- 性能测试报告（含数据图表）
- 开源仓库（含完整文档）
- 3-5 篇技术博客
- 面试问题清单与答案

---

## 三、每周时间分配建议

| 活动 | 时间 | 说明 |
|------|------|------|
| **受控实现与补全** | 8-12h/周 | 在任务卡边界内亲自完成 TODO，不做机械手抄 |
| **编译、单步调试和失败注入** | 4-6h/周 | 观察返回值、线程、句柄和错误路径 |
| **阅读文档与填写 API 卡片** | 3-5h/周 | Microsoft Learn + Windows Internals，逐渐自己完成选型 |
| **AI/人工代码审查后修正** | 2-4h/周 | 优先自己修改，不把审查等同于 AI 覆盖代码 |
| **复盘、博客和脱离答案重写** | 2-3h/周 | 用输出验证知识转化 |
| **社区交流** | 1-2h/周 | Reddit r/cpp, V2EX, 知乎 |

> **总计：约 25-30 小时/周**。如果你还有课业，建议压缩到 15-20 小时/周，将周期延长到 20-24 周。

---

## 四、推荐资源

### 书籍（按优先级）
| 书名 | 作者 | 用途 | 阅读方式 |
|------|------|------|---------|
| 《Windows 核心编程》第 5 版 | Jeffrey Richter | Win32 API 圣经 | 精读前 10 章 |
| 《Windows Internals》第 7 版 Part 1 | Mark Russinovich | 深入理解系统机制 | 按需查阅 |
| 《C++ Concurrency in Action》第 2 版 | Anthony Williams | 并发编程 | 精读前 5 章 |
| 《Effective Modern C++》 | Scott Meyers | 现代 C++ 最佳实践 | 精读 |

### 在线资源
| 资源 | 链接 | 用途 |
|------|------|------|
| Microsoft Learn | learn.microsoft.com | 官方文档，每日必查 |
| NT Internals | undocumented.ntinternals.net | Nt API 参考 |
| Sysinternals | docs.microsoft.com/sysinternals | 调试工具 |
| Raymond Chen 博客 | devblogs.microsoft.com/oldnewthing | Windows 设计哲学 |

### 工具链
| 工具 | 用途 | 获取 |
|------|------|------|
| Visual Studio 2022 | 编译调试 | 官网免费下载 |
| VS Code + Clangd | 主力编辑器 | 官网 + 扩展市场 |
| Process Monitor | API/文件/注册表监控 | Sysinternals |
| Process Explorer | 进程/句柄/线程分析 | Sysinternals |
| GPUView | GPU 调度分析 | Windows SDK |
| CapFrameX | 游戏帧时间分析 | GitHub 开源 |

---

## 五、关键决策点

### 5.1 编辑器选择：VS Code vs Vim

**推荐 VS Code**，理由：
- Windows 开发生态集成更好（CMake Tools, C/C++ 扩展）
- 调试体验远超 Vim（断点、内存查看、反汇编）
- 你可以保留 Vim 键位（VS Code Vim 扩展）
- 远程开发、Live Share 等现代功能

**保留 Vim 的场景**：
- 快速编辑配置文件
- SSH 到远程服务器时
- 你已经形成了肌肉记忆，切换成本太高

### 5.2 Linux vs Windows 开发

**主力 Windows**，理由：
- 目标平台就是 Windows，本地开发最准确
- Windows SDK、WDK 只能在 Windows 上安装
- 调试工具链（WinDbg, GPUView）Windows 独占

**Linux 的用途**：
- WSL2 中运行一些辅助脚本（Python 数据处理）
- 如果后续扩展 Linux 版本，已有环境
- Docker 容器化测试（虽然本项目用不到）

### 5.3 C++ 标准：17 vs 20

**推荐 C++20**，理由：
- `std::format` 替代 `printf`/`stringstream`，类型安全
- `std::jthread` 自动 join，避免线程泄漏
- `concepts` 让模板错误信息可读
- VS 2022 完整支持 C++20

**C++17 的保守选择**：
- 如果需要兼容旧编译器
- 第三方库不支持 C++20（但本项目依赖少）

---

## 六、检查清单（Checklist）

### 每周检查
- [ ] 本周代码提交到 Git（至少 3 次有意义的 commit）
- [ ] 本周至少解决一个"卡住的"问题（查文档/问社区/调试）
- [ ] 本周笔记/博客有进展（哪怕只是草稿）

### 每阶段检查
- [ ] Phase 1 结束：GitHub 仓库有内容，README 能说明项目做什么
- [ ] Phase 2 结束：有 2 个可独立运行的模块原型
- [ ] Phase 3 结束：完整程序能跑通，有配置文件
- [ ] Phase 4 结束：有数据、有博客、能面试

### 秋招前检查（10 月底）
- [ ] GitHub 仓库 Star > 10（发朋友圈/技术群求 Star）
- [ ] 博客阅读量 > 1000（发知乎/V2EX/掘金）
- [ ] 能 15 分钟讲清楚项目架构和核心难点
- [ ] 能回答 10 个以上的技术深挖问题

---

## 七、AI 协作阶段目标

### Phase 1

- 新领域默认 L0/L1：AI 给完整 API 卡片和最小安全示范；
- 学习者必须亲自调试、解释资源和完成一个小变体；
- 不以从零记住 API 为目标。

### Phase 2

- 常见 Win32 模式提升到 L2：AI 给骨架和 TODO；
- 学习者实现只读采样、错误判断和至少一个故障测试；
- Native API 仍由 AI 主导隔离和门禁。

### Phase 3

- Config、Policy 等低风险逻辑提升到 L3；
- R1 执行器采用共同实现，AI 主导租约、身份和恢复边界；
- 每个模块至少有一个学习者独立函数、测试和安全变体。

### Phase 4

- 学习者能够进行 L4 API/架构比较；
- AI 主要负责质询、安全审查和证据检查；
- 是否掌握以脱离答案重写、失败路径和实验报告判断，不以代码行数判断。

## 八、心态建议

> **"不要把这个项目当'一个工具'做，要当'一张门票'做。"**

1. **进度焦虑是正常的**：每周只完成计划的 70% 也是胜利
2. **完美是完成的敌人**：先让程序跑起来，再优化代码质量
3. **输出倒逼输入**：每周写博客，哪怕只有 300 字
4. **展示比隐藏好**：尽早开源，社区反馈是最佳导师
5. **面试是双向选择**：这个项目帮你筛选出"愿意聊底层"的公司，这些公司通常技术氛围更好

---

*文档版本：v1.1 | 创建日期：2026-08-08 | 更新日期：2026-08-09 | 适用周期：按实际学习证据动态调整*
