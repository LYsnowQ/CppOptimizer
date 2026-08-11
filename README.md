# CppOptimizer

Windows x64 用户态系统性能观测与受控优化工具。

以正式软件工程标准开发，当前阶段聚焦**只读观测**：内存快照、有界观测窗口与平台能力探测。项目不承诺在任意设备或游戏上提升性能；任何优化动作都必须先具备可测量、可撤销的证据。

## 特性

- **只读内存观测**
  - `--status`：单次物理内存快照（total / available / used / load / age）
  - `--observe <seconds>`：前台有界观测窗口（1–60 秒），输出负载 min/avg/max 与可用内存 min/max
- **平台诊断**：`--diagnose` Native API 能力探测（只读）
- **工程基础**：统一错误域模型（`Result<T>` / `Error`）、RAII 资源所有权、C++20、CTest 单元测试

## 设计原则

1. 安全与正确性优先于性能收益；
2. 先观测、再决策、最后执行；
3. 标准用户运行是默认路径；
4. 系统级动作必须经过多重门禁、租约、审计与恢复；
5. 缺少配对 A/B 数据时，不默认启用优化；
6. 不以“可用内存增加”或“频率更高”单独证明游戏性能改善。

## 安全边界

- 当前所有命令均为只读查询，不修改系统状态；
- 不注入、不 Hook、不读写游戏内存、不加载内核驱动；
- 自动内存清理（Standby/Modified List 等）、全局电源修改、进程优先级调整等高风险动作默认关闭；
- 调用 `Nt*` API 不等于编写内核驱动；本项目当前全部位于用户态（Ring 3）。

## 构建

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022（安装“使用 C++ 的桌面开发”与 Windows SDK）
- C++20

### 方式一：Visual Studio

打开 `CppOptimizer.slnx`，选择 `Debug|x64` 或 `Release|x64`，生成解决方案。

### 方式二：CMake + Ninja

```powershell
cmake --preset windows-x64-debug
cmake --build --preset build-debug
ctest --preset test-debug
```

## 使用

```text
CppOptimizer.exe --diagnose     平台与 Native API 能力探测（只读）
CppOptimizer.exe --status       单次只读内存快照
CppOptimizer.exe --observe <s>  每秒采样内存并输出窗口报告（1–60 秒，前台有界，只读）
CppOptimizer.exe --help         帮助信息
```

示例输出（数值随机器状态变化）：

```text
> CppOptimizer.exe --status
Memory snapshot (read-only, single query)
  total      : 31.6 GiB (33968340992 bytes)
  available  : 13.0 GiB (13936865280 bytes)
  used       : 18.7 GiB (20031475712 bytes)
  load       : 58%
  age        : 0 ms (fresh: yes)

> CppOptimizer.exe --observe 3
Memory observation window (read-only, foreground, 3 s)
  samples      : 3
  load %       : min 61 / avg 61 / max 61
  available    : min 12.2 GiB / max 12.2 GiB
```

## 测试

```powershell
ctest --preset test-debug
```

当前覆盖：错误模型与资源所有权、内存快照契约（输入校验、`used` 派生、`available == total` 边界）、字节显示与快照时效边界、观测窗口聚合（空窗口 / 越界错误路径、round-half-up、顺序无关、整数溢出安全）。

## 项目状态与路线图

**当前阶段**：工程基线与只读观测。

- 已完成：统一错误模型、RAII 资源封装、Native API 只读能力探测、内存只读快照与字节格式化、`--observe` 观测窗口聚合；
- 规划中：Logger → ConfigManager → 指标采集（PDH）→ ProcessWatcher → PolicyEngine 只读决策 → 低风险执行（PowerLocker / PriorityBooster）→ Agent/Service 形态；
- 实验性：内存清理、GPU 心跳、调度调整等模块默认关闭，仅在门禁、测试与审计就绪后评估。

## 目录结构

```text
include/   公共头文件（模块契约与不变量）
source/    实现
tests/     单元测试
config/    示例配置
```

## 贡献与安全

- 开发流程：[CONTRIBUTING.md](CONTRIBUTING.md)
- 安全报告：[SECURITY.md](SECURITY.md)
