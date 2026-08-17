# CppOptimizer

Windows x64 用户态系统性能观测与受控优化工具。

以正式软件工程标准开发，当前阶段聚焦**只读观测**：内存快照、有界观测窗口与平台能力探测。项目不承诺在任意设备或游戏上提升性能；任何优化动作都必须先具备可测量、可撤销的证据。

## 特性

- **只读内存观测**
  - `--status`：单次物理内存快照（total / available / used / load / age）
  - `--observe <seconds>`：前台有界观测窗口（1–60 秒），输出负载 min/avg/max 与可用内存 min/max
- **平台诊断**：`--diagnose` Native API 能力探测（只读）
- **中文环境支持**：面向中文 Windows，内部宽字符（UTF-16）、日志/存储 UTF-8、控制台/日志/错误消息均可承载中文（见工程手册 8.1）
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

### 输出目录约定（out/）

`out/` 为共同父目录，两种构建方式分开存储，互不干扰：

```text
out/
├── cmake/                  # CMake/Ninja 构建（CMakePresets binaryDir）
│   └── windows-x64-debug|release/
└── msvc/                   # Visual Studio (MSBuild) 构建（vcxproj OutDir）
    └── x64/Debug|Release/
```

- MSVC 构建后，PostBuildEvent 会把 `thirdParty/<库>/lib/` 中存在的动态库（DLL）复制到输出目录（与 CMake 行为一致）；当前 spdlog 为静态库，无 DLL 可复制，规则为空操作；
- 根目录不存放任何构建输出（`x64/`、`error.obj` 等残留已移除）。

### 第三方库（thirdParty）

- 所有三方库统一放在 `thirdParty/<库名>/` 目录下（如 `thirdParty/spdlog/`）；该目录已被 `.gitignore` 忽略，不提交 Git；
- **目录约定**：每个库自含 `include/`（头文件）、`lib/`（编译产物：静态/动态库 + PDB）、`src/` + `CMakeLists.txt`（保留源码，便于重新编译）三个部分；第三方库文件与项目输出（`out/`）不混放；
- **源码处理**：下载源码后先编译，编译产物（`.lib`/`.dll`/`.pdb`）复制回该库自己的 `lib/` 目录；源码与构建脚本（`src/`、`cmake/`、`CMakeLists.txt`）**保留**以便未来重编或升级；
- **引用方式**：包含路径配置为 `include` 与 `thirdParty` 两个根（另含 `thirdParty/<库>/include` 供库内部互引解析，如 spdlog 头文件间 `<spdlog/...>`）；代码中**写完整路径**便于知晓库的具体位置，例如 `#include <spdlog/include/spdlog/spdlog.h>`；
- spdlog 为 compiled 模式静态库：CMake 按配置链接 `lib/spdlogd.lib`（Debug）/ `lib/spdlog.lib`（Release），vcxproj 的 Link 同样按配置链接；两套构建均定义 `SPDLOG_COMPILED_LIB` 并加 `/utf-8`（spdlog bundled fmt 要求）。
- **toml++ v3.4.0**（配置解析）为 **header-only** 库（MIT）：`thirdParty/tomlplusplus/include/` 仅头文件，无编译产物；引用 `#include <toml++/toml.h>`（完整路径）。选型理由：TOML 面向玩家可读、支持注释（安全护栏）、可表达嵌套/数组（`[[games]]`）、不易写坏（对比 JSON 少逗号即全废）；JSON 留给未来 Web 界面（程序间传输），环境变量/命令行只做覆盖层不做主配置。

## 使用

```text
CppOptimizer.exe --diagnose     平台与 Native API 能力探测（只读）
CppOptimizer.exe --status       单次只读内存快照
CppOptimizer.exe --observe <s> [threshold]  每秒采样并输出窗口报告（1–60 秒，前台有界，只读；可选低负载阈值 0..100，默认 50）
CppOptimizer.exe --log <module> <message...>  写一条 Info 日志到 stderr（同步，只读）
CppOptimizer.exe --config <path>  解析并校验 TOML 配置文件（只读）
CppOptimizer.exe --cpu           采样 CPU 使用率（只读，PDH）
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
  load < 50%   : 100% of samples

> CppOptimizer.exe --observe 5 80
Memory observation window (read-only, foreground, 5 s)
  samples      : 5
  load %       : min 58 / avg 60 / max 63
  available    : min 12.1 GiB / max 12.4 GiB
  load < 80%   : 100% of samples

> CppOptimizer.exe --log memory "hello"
2026-08-13 20:21:04.746 [INFO] memory: hello

> CppOptimizer.exe --config config\config.example.toml
Config snapshot (read-only)
  version    : 1
  mode       : observe
  logging    : level info, max 10 MB x 5 files
  memory     : query on, clean off, native-write off
```

## 测试

```powershell
ctest --preset test-debug
```

当前覆盖：错误模型与资源所有权、内存快照契约（输入校验、`used` 派生、`available == total` 边界）、字节显示与快照时效边界、观测窗口聚合（空窗口 / 越界错误路径、round-half-up、顺序无关、整数溢出安全）、低负载占比（严格小于语义、阈值 0/100 边界、round-half-up）、结构化日志（级别过滤、格式化纯函数、文件 sink 与 RAII 关闭、失败降级不递归、并发写）。

## 项目状态与路线图

**当前阶段**：工程基线与只读观测。

- 已完成：统一错误模型、RAII 资源封装、Native API 只读能力探测、内存只读快照与字节格式化、`--observe` 观测窗口聚合与低负载占比、结构化日志器（同步 sink、级别过滤、降级路径）、配置解析与校验（`--config`，toml++）、PDH 只读采样（`--cpu`）；
- 规划中：Logger -> ConfigManager -> 指标采集（PDH）-> ProcessWatcher -> PolicyEngine 只读决策 -> 低风险执行（PowerLocker / PriorityBooster）-> Agent/Service 形态；
- 实验性：内存清理、GPU 心跳、调度调整等模块默认关闭，仅在门禁、测试与审计就绪后评估。

## 目录结构

```text
include/       公共头文件（模块契约与不变量）
source/        实现
thirdParty/    三方库（include + lib + 源码，本地依赖，不提交 Git）
tests/         单元测试
config/        示例配置
logs/          运行时日志收容目录（不提交 Git）
out/           构建输出（cmake/ 与 msvc/ 分开，不提交 Git）
```

## 贡献与安全

- 开发流程：[CONTRIBUTING.md](CONTRIBUTING.md)
- 安全报告：[SECURITY.md](SECURITY.md)
