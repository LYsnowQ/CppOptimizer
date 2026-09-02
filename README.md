# CppOptimizer

Windows x64 用户态系统性能观测与受控优化工具。

以正式软件工程标准开发，当前阶段聚焦**只读观测**：内存快照、有界观测窗口与平台能力探测。项目不承诺在任意设备或游戏上提升性能；任何优化动作都必须先具备可测量、可撤销的证据。

## 特性

- **只读内存观测**
  - `--status`：单次物理内存快照（total / available / used / load / age）
  - `--observe <seconds>`：前台有界观测窗口（1–60 秒），输出负载 min/avg/max 与可用内存 min/max
- **只读 CPU 采样**：`--cpu` PDH 采样（两次采样、速率节奏契约）
- **进程生命周期观测**：`--watch <seconds> [config.toml]` 按配置的游戏规则轮询 Toolhelp 进程快照，输出 `NotRunning/Starting/Running/Exiting` 状态转移（PID + 创建时间双重身份防 PID 重用）
- **进程目录**：`--list-processes [--all] [filter]` 只读列出运行进程（pid/路径/窗口标题/前台/内存），供辨认并挑选要添加为游戏的进程；支持中文进程名与窗口标题
- **自选进程添加游戏**：`--add-game [pid] [main.toml] [--dry-run]` 从运行进程自动生成游戏规则（id 从进程名派生并去重、display_name 取窗口标题），写入用户自建配置 `config.local.toml`（与预设配置分离，原子写）；交互模式（无 pid）列出有窗口进程供编号选择
- **只读策略决策 + 门禁执行**：`--policy <s> [config.toml]` 消费内存余量与游戏焦点（ProcessWatcher 前台轮询），按 `[policy]` 阈值分级（Comfortable/Adequate/Tight/Critical）并评估规则（无游戏 no_game / 危急仅提示 mem_critical / 后台暂停不优化 game_background / 紧张建议内存维护 mem_tight / 前台余量充足建议优先级提升 prio_boost），附防抖冷却抑制抖动；PWR-002 起，决策经配置门禁落地为 R1 动作（`[priority].enabled` -> 前台游戏提升优先级；`[power].execution_required` -> 游戏运行期持有电源请求），游戏退出自动释放；无配置或门禁关闭时保持纯咨询不产生任何系统修改
- **R1 局部可逆电源请求**：`--power-lock <s> [execution|display|both] [reason...]` 前台有界持有 Windows Power Request（`execution` 阻止睡眠 / `display` 阻止熄屏），到点自动释放，进程退出时句柄随句柄表关闭、系统侧请求自动取消；Power Request 表达睡眠/显示需求，不承诺锁定 CPU/GPU 频率
- **R1 局部可逆优先级提升**：`--priority-boost <s> <pid> [config.toml]` 对指定进程临时提升优先级类（等级取自 `[priority].max_level`，默认 AboveNormal，High 需显式配置；realtime 在配置层拒绝），到点条件恢复——仅当进程仍同实例且当前优先级未被外部改动时才恢复原值（不覆盖外部修改）；目标退出视为正常取消
- **平台诊断**：`--diagnose` Native API 能力探测（只读）
- **中文环境支持**：面向中文 Windows，内部宽字符（UTF-16）、日志/存储 UTF-8、控制台/日志/错误消息均可承载中文（见工程手册 8.1）
- **工程基础**：统一错误域模型（`Result<T>` / `Error`）、RAII 资源所有权、C++20、CTest 单元测试
- **受保护命名管道 IPC**（IPC-001 传输 + IPC-002 结构化载荷，批次 4 运行形态续）：`--ipc-pipe server <s> [suffix]` 前台有界服务端——显式 SDDL 命名管道（仅 SYSTEM/管理员/交互用户）、固定 16 字节帧头（magic/版本/类型/长度/request ID）、载荷上限 4096 字节、读/写超时、未知版本/类型/超长载荷帧级拒绝（不做宽松转换）、记录客户端 PID + 会话；`--ipc-pipe client [suffix]` 客户端——连接后发送一帧 FactsSnapshot（载荷为 CPOPFACTS/1 结构化 UTF-8 事实：信封行 + `key=value` 行，键 ASCII 字母/数字/`_`/`-`、值 0..128 字节且禁控制字符、去重、至多 64 条，任一违反整体拒绝）并校验应答（requestId 配对、Error 应答不伪装成功）；服务端默认处理器解析结构化事实，合法回 Ack（紧凑摘要，超限截断）、违反契约回 Error(InvalidFacts)；单请求-应答握手（断开前服务端等待客户端关闭）；键语义白名单与会话校验属后续切片

## 设计原则

1. 安全与正确性优先于性能收益；
2. 先观测、再决策、最后执行；
3. 标准用户运行是默认路径；
4. 系统级动作必须经过多重门禁、租约、审计与恢复；
5. 缺少配对 A/B 数据时，不默认启用优化；
6. 不以“可用内存增加”或“频率更高”单独证明游戏性能改善。

## 安全边界

- 当前除 `--power-lock`（R1 局部可逆、前台有界、退出自动释放）、`--priority-boost`（R1 局部可逆、前台有界、条件恢复、不覆盖外部修改）与 `--policy` 在配置门禁开启时的 R1 落地（`[priority].enabled` / `[power].execution_required`，游戏退出自动释放）外，所有命令均为只读查询，不修改系统状态；
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
CppOptimizer.exe --watch <s> [config.toml]  按配置的游戏规则观测进程生命周期（只读，Toolhelp，1–60 秒）
CppOptimizer.exe --list-processes [--all] [filter]  列出运行进程（只读；默认仅有可见窗口者）
CppOptimizer.exe --add-game [pid] [main.toml] [--dry-run]  从运行进程添加游戏规则到 config.local.toml（无 pid 时交互选择）
CppOptimizer.exe --policy <s> [config.toml]  决策窗口（1–60 秒；门禁开启时落地 R1 执行，门禁全关为纯咨询）
CppOptimizer.exe --power-lock <s> [execution|display|both] [reason...]  持有电源请求（R1，前台有界 1–60 秒，退出自动释放）
CppOptimizer.exe --priority-boost <s> <pid> [config.toml]  临时提升进程优先级类（R1，前台有界 1–60 秒，条件恢复；等级取自 [priority].max_level）
CppOptimizer.exe --ipc-pipe server <s> [suffix]  受保护命名管道服务端（前台有界 1–60 秒，至多服务一个客户端一帧；FactsSnapshot 载荷按 CPOPFACTS/1 解析回摘要，违反契约回 Error）
CppOptimizer.exe --ipc-pipe client [suffix]  受保护命名管道客户端（发送 CPOPFACTS/1 结构化 FactsSnapshot 帧并打印应答摘要）
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

> CppOptimizer.exe --watch 3 config\config.example.toml
Process watcher (read-only, foreground, 3 s)
  rules : 1 game rule(s)
  [NotRunning -> Starting] example-game pid=12708 ExampleGame.exe
  [Starting -> Running] example-game pid=12708 ExampleGame.exe
  tracked : 1 process(es)
    [example-game] ExampleGame.exe pid=12708 Running

> CppOptimizer.exe --list-processes code
Process list (read-only, visible windows)
  pid     name        path                                     window title          memory
  13876   devenv.exe  C:\SoftWare\IDE\Microsoft VS\Common7\IDE\devenv.exe  CppOptimizer - config.exam  922.8 MiB

> CppOptimizer.exe --add-game 14648
将添加游戏规则：
  id             : explorer
  display_name   : Program Manager
  process_names  : ["explorer.exe"]
  pause_when_background : true
  已添加规则 -> config.local.toml

> CppOptimizer.exe --watch 3
Process watcher (read-only, foreground, 3 s)
  rules : 1 game rule(s)
  [NotRunning -> Starting] explorer pid=14648 explorer.exe
  [Starting -> Running] explorer pid=14648 explorer.exe
```

```text
> CppOptimizer.exe --policy 5 config\config.example.toml
Policy decision (read-only advisory, no system changes)
  rules : 1 game rule(s)
  [1s] margin 28% adequate game=none -> NoOp (no_game)
  [2s] margin 28% adequate game=self fg=no -> Notify (mem_critical)
  [3s] margin 29% critical game=self fg=no -> Notify (mem_critical)
  summary : NoOp 1 / Notify 2 / SuggestMemoryTune 0 / SuggestPriorityBoost 0
```

```text
> CppOptimizer.exe --policy 6 config\pwr002-demo.toml   （[priority].enabled / [power].execution_required 开启时）
Policy decision window (advisory; R1 executors active)
  exec     : priority on, power on
  rules : 1 game rule(s)
  [1s] margin 57% comfortable game=blender-demo fg=yes -> SuggestPriorityBoost (prio_boost)
  [exec] priority boosted: blender-demo pid=7016
  [exec] power request held (game running)
  [6s] margin 56% comfortable game=blender-demo fg=no -> NoOp (game_background)
  [exec] priority released: blender-demo
  summary : NoOp 1 / Notify 0 / SuggestMemoryTune 0 / SuggestPriorityBoost 5
  exec     : boost 1 / unboost 1 / power+ 1 / power- 0
```

```text
> CppOptimizer.exe --service console 3
Service host (console mode, R0 workload, 3 s)
  workload : memory snapshot + info log (read-only)
  stop     : Ctrl+C or timeout
2026-08-26 10:25:40.665 [INFO] service: tick 1: available 18.5 GiB, load 41%
2026-08-26 10:25:41.668 [INFO] service: tick 2: available 18.6 GiB, load 41%
2026-08-26 10:25:42.677 [INFO] service: tick 3: available 18.5 GiB, load 41%
  ticks    : 3
  stopped  : timeout

> CppOptimizer.exe --service install
  service install failed [Win32:5] 拒绝访问。 (需要管理员权限)
```

```text
> 终端 A：CppOptimizer.exe --ipc-pipe server 30
IPC pipe server (protected transport, single client, 30 s)
  pipe     : \\.\pipe\CppOptimizerIpc
  client   : pid 13384, session 1
  request  : FactsSnapshot id=1 payload=59 bytes
  reply    : ack sent (facts parsed and summarized)

> 终端 B：CppOptimizer.exe --ipc-pipe client
IPC pipe client (protected transport)
  pipe     : \\.\pipe\CppOptimizerIpc
  request  : FactsSnapshot id=1 payload=59 bytes (2 facts)
  facts    : facts ok (2): client_pid=13384 observer=CppOptimizer ipc demo
  reply    : Ack id=1 payload=61 bytes [facts ok (2): client_pid=13384 observer=CppOptimizer ipc demo]
```

## 测试

```powershell
ctest --preset test-debug
```

当前覆盖：错误模型与资源所有权、内存快照契约（输入校验、`used` 派生、`available == total` 边界）、字节显示与快照时效边界、观测窗口聚合（空窗口 / 越界错误路径、round-half-up、顺序无关、整数溢出安全）、低负载占比（严格小于语义、阈值 0/100 边界、round-half-up）、结构化日志（级别过滤、格式化纯函数、文件 sink 与 RAII 关闭、失败降级不递归、并发写）、PDH 采样（warming-up、节奏契约）、进程生命周期（名称匹配、规则匹配、状态差分全状态机、PID 重用/重启、窗口/创建时间查询、轮询线程事件投递）、进程目录（详情查询、窗口过滤、子串匹配）、规则生成与配置写入（id 派生/去重、TOML 转义、原子写、main+local 合并）、宽字符控制台输出（UTF-8 往返）、策略决策（余量计算与分级边界、规则评估全分支含 prio_boost、防抖冷却语义、求值器组合、`[policy]` 配置校验）、电源请求（可注入 fake 的引用计数状态机：配对释放、幂等、失败路径、RAII 自动释放、类型解析）、优先级提升（可注入 fake 的租约状态机：最小权限、身份重验、条件恢复不覆盖外部修改、失败不伪装成功、目标退出视为取消、max_level 门禁）、策略执行器（可注入双 fake 的期望状态对账：门禁开关、幂等、目标变化替换、游戏退出自动释放、电源生命周期、失败路径、RAII）、服务宿主（运行模式解析、状态机合法/非法转移、上报构造与控制码、可注入 SCM fake 的服务状态序列、控制码分支、负载失败/上报失败不伪装、控制台生命周期、停止幂等粘性、安装/卸载参数校验、真实后端非 SCM 启动失败路径）、受保护命名管道 IPC（帧头构造/严格解析与校验全项、长度边界、序列化往返、可注入双 fake 的单帧会话：Ping/Ack 与 FactsSnapshot 应答、自定义处理器、非法帧/未知版本/未知类型/超长载荷拒绝并回 Error、接受超时、创建/读写失败不伪装、处理器失败回 Error、默认处理器拒绝、客户端往返 requestId 配对与 Error 应答不伪装、连接/读写失败路径；IPC-002 Facts 载荷契约：序列化/解析往返与首行信封、多字节 UTF-8 值往返、信封缺失/版本不符/旧格式/空载荷/尾随换行/空行/CR/缺 '='/空键/非法键字符/重复键/控制字节/键值超限/条数超限整体拒绝、编码同规则拒绝非法输入、摘要格式与超限截断有界、违反契约的 FactsSnapshot 回 Error(InvalidFacts)）。

## 项目状态与路线图

**当前阶段**：工程基线与只读观测。

- 已完成：统一错误模型、RAII 资源封装、Native API 只读能力探测、内存只读快照与字节格式化、`--observe` 观测窗口聚合与低负载占比、结构化日志器（同步 sink、级别过滤、降级路径）、配置解析与校验（`--config`，toml++）、PDH 只读采样（`--cpu`）、进程生命周期观测（`--watch`，Toolhelp 轮询 + 窗口检测 + PID/创建时间身份）、进程目录（`--list-processes`，路径/窗口/内存详情）、自选进程添加游戏闭环（`--add-game`，规则自动生成 + `config.local.toml` 原子写 + main/local 合并加载）、PolicyEngine 只读决策（`--policy`，压力分级 + 规则评估 + 防抖，`[policy]` 配置节）、PowerLocker 首切片（`--power-lock`，电源请求引用计数状态机 + 可注入后端 + R1 可逆演示）、PriorityBooster 首切片（`--priority-boost`，租约状态机 + 条件恢复 + 可注入后端 + R1 可逆演示）、PolicyEngine 接入执行器（`--policy` 决策经 `[priority]`/`[power]` 门禁落地 R1 动作：前台游戏提升 + 游戏运行期电源请求，游戏退出自动释放，无配置或门禁全关纯咨询）、ServiceHost 首切片（`--service console/install/uninstall` + SCM 入口：控制台/服务双模式宿主、SCM 状态机与安装卸载、可注入后端、R0 只读负载）；
- 规划中：Agent/Service 运行形态（控制台宿主 -> ServiceHost / Per-user Agent）；ServiceHost 首切片已落地（SVC-001），受保护 IPC 传输（IPC-001）与结构化 Facts 载荷（IPC-002）已落地，Agent 键语义白名单、会话校验与 Service 端消费属后续切片；
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
