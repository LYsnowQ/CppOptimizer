# CppOptimizer

Windows x64 用户态系统性能观测与受控优化工具。

以正式软件工程标准开发，当前阶段聚焦**只读观测**：内存快照、有界观测窗口与平台能力探测。项目不承诺在任意设备或游戏上提升性能；任何优化动作都必须先具备可测量、可撤销的证据。

## 特性

- **只读内存观测**
  - `--status`：单次物理内存快照（total / available / used / load / age）
  - `--observe <seconds>`：前台有界观测窗口（1–60 秒），输出负载 min/avg/max 与可用内存 min/max
- **只读 CPU 采样**：`--cpu` PDH 采样（两次采样、速率节奏契约）
- **进程生命周期观测**：`--watch <seconds> [config.toml]` 按配置的游戏规则轮询 Toolhelp 进程快照，输出 `NotRunning/Starting/Running/Exiting` 状态转移（PID + 创建时间双重身份防 PID 重用）；游戏规则的 `window_title_contains` 为主窗口标题子串过滤——同名多实例或启动器与游戏同进程名时，只命中主窗口标题含该子串的进程，无可见窗口或窗口标题未知的进程不命中（空串保持仅按进程名匹配，行为不变；`--policy` 复用同一规则匹配）
- **进程目录**：`--list-processes [--all] [filter]` 只读列出运行进程（pid/路径/窗口标题/前台/内存），供辨认并挑选要添加为游戏的进程；支持中文进程名与窗口标题
- **自选进程添加游戏**：`--add-game [pid] [main.toml] [--dry-run]` 从运行进程自动生成游戏规则（id 从进程名派生并去重、display_name 取窗口标题），写入用户自建配置 `config.local.toml`（与预设配置分离，原子写）；交互模式（无 pid）列出有窗口进程供编号选择
- **只读策略决策 + 门禁执行**：`--policy <s> [config.toml]` 消费内存余量与游戏焦点（ProcessWatcher 前台轮询），按 `[policy]` 阈值分级（Comfortable/Adequate/Tight/Critical）并评估规则（无游戏 no_game / 危急仅提示 mem_critical / 后台暂停不优化 game_background / 紧张建议内存维护 mem_tight / 前台余量充足建议优先级提升 prio_boost），附防抖冷却抑制抖动；决策在配置门禁开启时落地为 R1 动作（`[priority].enabled` -> 前台游戏提升优先级；`[power].execution_required` -> 游戏运行期持有电源请求），游戏退出自动释放；无配置或门禁关闭时保持纯咨询不产生任何系统修改。`[policy].user_away_idle_seconds > 0` 开启**用户在场门禁**（ACT-004）：每秒只读查询最近输入（GetLastInputInfo），无输入达到阈值视用户不在场（AFK；锁屏/断开因输入时钟冻结自然落入）-> `NoOp(user_away)` 抑制一切优化建议（危急 Notify 与无游戏不受影响）；查询失败同样视不在场（不优化是安全方向）；0（默认）关闭，行为零回归；`[policy].halt_after_action_failures > 0`（IPC-017）开启 R1 动作连续失败停摆：连续 N 次优先级/电源动作失败后执行器停摆、不再触碰系统（决策保持纯咨询，ResetHalt 恢复）
- **R1 局部可逆电源请求**：`--power-lock <s> [execution|display|both] [reason...]` 前台有界持有 Windows Power Request（`execution` 阻止睡眠 / `display` 阻止熄屏），到点自动释放，进程退出时句柄随句柄表关闭、系统侧请求自动取消；Power Request 表达睡眠/显示需求，不承诺锁定 CPU/GPU 频率
- **R1 局部可逆优先级提升**：`--priority-boost <s> <pid> [config.toml]` 对指定进程临时提升优先级类（等级取自 `[priority].max_level`，默认 AboveNormal，High 需显式配置；realtime 在配置层拒绝），到点条件恢复——仅当进程仍同实例且当前优先级未被外部改动时才恢复原值（不覆盖外部修改）；目标退出视为正常取消
- **用户输入活动观测**：`--activity <s> [idle-secs] [--session] [--foreground]` 前台有界只读观测最近键鼠输入（`GetLastInputInfo`，无 Hook、不采集输入内容）——每秒采样分类 Active/Idle/Unknown（空闲阈值秒数可调，默认 15s；非交互会话查询失败按 Unknown 降级不伪装），输出逐样本状态与窗口汇总；GetTickCount 32 位回绕与阈值边界经纯函数单测覆盖。`--session`（会话上下文，R0 只读 WTS）追加远程会话标志（`SM_REMOTESESSION`）、锁屏状态（`WTSSessionInfoEx` SessionFlags）与会话连接状态（`WTSConnectState`）：断开会话/锁屏覆盖输入态为 Disconnected/Locked，输入或会话任一查询失败整样本按 Unknown 降级不伪装；实测本地交互会话正常输出 local session + Active/Idle，锁屏/RDP 场景可配 --session 人工复核。`--foreground`（前台归属，ACT-003，R0 只读 `GetForegroundWindow`+`GetWindowThreadProcessId`，无新库）在 Active/Idle 样本上追加前台窗口所属进程（`fg=<pid>`，无前台窗口 `fg=none`，查询失败 `fg=unavailable` 不伪造）：Locked/Disconnected/Unknown 样本不可归属（安全桌面/无交互/不知在场，不输出前台）；可与 --session 同时使用且顺序无关，实测连续输出同一前台进程 pid
- **受控优化宿主控制台消费**：`--service console <s> --ipc-facts` 在宿主负载窗口内作为**受保护管道服务端常驻监听**（`IpcSession.persistentAccept`，实例跨 tick 保持、无监听空窗）连续受理到达的 Agent 客户端（复用命名管道全链路：帧校验、事实解析与键白名单、会话裁决、用户 SID 采集与凭据校验），逐客户端记录身份（pid/会话/用户 SID）与事实摘要并回 Ack（R0 只读）；实测连续受理两个客户端且均无需重连；同一连接内多帧会话、连接复用与多实例并发受理（见下）亦已落地；ACT-006 起逐客户端展示 Agent 上报的用户活动（`user idle=N s`，user_idle_seconds 经通用数值访问器读取，未上报按 n/a 不伪装；窗口汇总同样展示）——为常驻编排/Safe Mode 联动提供在场度观测种子（消费端判定属后续）；宿主带 **Safe Mode 受理门禁**（时间窗口内身份/凭据失败达阈值即暂停受理新 Agent、冷却到期自动恢复并清窗；正常受理不冲销失败计数——IPC-013 窗口计数语义）；门禁亦支持启动环境离散异常锁存触发（IPC-015/016：Native capability 探测异常、不支持的 OS/build——受理前只读启动基线（Native 探测 + RtlGetVersion 系统版本 + x64 判定）任一异常即进入 Safe Mode 并锁存保持整个窗口，ClearAnomaly 显式恢复；异常退出恢复确认等其余触发项随消费方落地）；IPC-018 起会话级恢复标记：`--service console --ipc-facts` 开始写 recovery-state.json（%LOCALAPPDATA%\CppOptimizer\）、正常结束清除；上次会话异常退出（标记留存）未确认即锁存 Safe Mode（暂停受理），`--confirm-recovery` 显式确认后继续（只读 R0 不受影响）；门禁参数（阈值/窗口/冷却/开关）可经 `[ipc]` 配置节调整（`--service console <s> [config.toml] --ipc-facts`，缺省 3 次/5s/冷却 2s、启用；越界或为零配置直接拒绝，宿主 banner 与门禁随实际参数生效）；SVC-004 起支持常驻与默认配置：`<s>` 可用 `run` 表示常驻（无时间上限，直到 Ctrl+C/关闭信号优雅停止；异常退出由恢复标记在下次启动检测）；无显式配置时自动消费每用户默认配置 `%LOCALAPPDATA%\CppOptimizer\config.local.toml`（与恢复标记/凭据同目录）——存在且合法即生效 [ipc]，存在但无效 -> Safe Mode「配置无效」离散锁存（暂停受理 Agent、R0 负载照常，修复或移除后重启恢复；显式给出的无效配置仍直接拒绝）；SVC-005 起支持托盘形态：加 `--tray` 在通知区显示图标——独立 UI 线程创建隐藏窗口 + 消息循环，右键菜单“退出”回调请求宿主优雅停止（Ctrl+C/托盘退出/超时均统一收尾并移除图标）；图标添加失败显式报错不遗留线程；菜单交互需桌面人工点验（图标添加与退出消息路径经单测/实测覆盖）；SVC-006 起宿主把受理的 Agent 用户活动汇总为**在场状态**（`ClassifyPresence`/`HostPresenceTracker` 纯逻辑：空闲达阈值视不在场、未上报 Unknown、任一在场即宿主在场；逐客户端日志标注 Present/Away/Unknown + 窗口汇总 presence 行；仅观测/记录，不参与 Safe Mode 触发）；SVC-007 起阈值与政策口径一致（[policy].user_away_idle_seconds > 0 时采用其值否则默认 15s；`EffectivePresenceAwaySeconds` 纯函数），客户端键含会话号（pid:session），汇总行带 present/away/unknown 计数；SVC-008 起 --tray + --ipc-facts 时宿主经托盘隐藏窗口注册 **Raw Input 事件驱动输入**（键盘+鼠标，RIDEV_INPUTSINK 非前台也接收；只记时刻不读内容）：在场台账增加 [host] 行（事件优先、未绑定/尚无事件回退 GetLastInputInfo 轮询），汇总 input 行标注 raw sink attached；SVC-009 起在场汇总状态变化（Unknown/Present/Away 间转移）逐次记日志（在场度时间线）并在 presence 汇总行带 transitions 计数；SVC-010 起转移同时追加到每用户时间线文件 %LOCALAPPDATA%\CppOptimizer\presence-timeline.log（一行一条本地时间戳 + 状态转移，追加写/父目录自建/失败如实上报）并展示路径；IPC-019 起 Agent 周期上报另附前台窗口归属 `foreground_pid`（GetForegroundWindow 只读，无前台/查询失败省略不伪造；已注册数值键 >= 1），宿主逐客户端与窗口汇总展示 `fg pid`
- **Agent 周期上报实体（客户端）**：`--agent run <s> [suffix] [--interval-ms <50..10000>]` 前台有界周期上报——每周期采集真实内存观测经受保护命名管道上报 FactsSnapshot，ACT-005 起另附用户活动观测事实 `user_idle_seconds`（GetLastInputInfo 只读：距最近键鼠输入秒数，锁屏/断开输入时钟冻结自然增长；查询失败省略不伪装；经宿主 v1 键语义白名单接受并计入回显）（可选 agent_token 凭据），请求-应答逐次配对、Error 应答如实计入失败；宿主离线/Safe Mode 暂停等连接失败**有界重试与退避**（单次上报至多 3 次连接尝试），窗口到期必然返回汇总（sent / connect failures），不会无限重连；**自适应上报节奏**（`--max-interval-ms`，缺省 max(3×interval, 5s)）——连续失败后逐次拉大周期间隔至上限（宿主离线/Safe Mode 暂停时长离线时不高频空试），恢复成功即回到基础周期；ACT-007 在场感知节奏（`--away-after-seconds`/`--away-cap-ms`）：用户空闲达阈值（锁屏/断开输入时钟冻结亦自然落入）逐次拉大上报间隔至封顶、恢复输入即回基础周期（与失败退避正交取大，缺省关闭零回归）
- **平台诊断**：`--diagnose` Native API 能力探测（只读）
- **中文环境支持**：面向中文 Windows，内部宽字符（UTF-16）、日志/存储 UTF-8、控制台/日志/错误消息均可承载中文（直连控制台按宽字符直写，重定向到文件/管道按 UTF-8 字节输出）
- **工程基础**：统一错误域模型（`Result<T>` / `Error`）、RAII 资源所有权、C++20、CTest 单元测试、审计记录（`optimizer::audit`：`--policy` 门禁开启执行 R1 动作时按审计字段记录——operation/risk/caller/target/detail/ok/时刻，进程内有界环形保存并输出窗口审计行；为“审计不可用且请求 R2/R3”Safe Mode 源铺路）
- **受保护命名管道 IPC**（受保护传输 + 结构化载荷 + 键语义白名单 + 会话裁决 + 凭据认证 + 用户 SID 授权）：`--ipc-pipe server <s> [suffix]` 前台有界服务端——显式 SDDL 命名管道（仅 SYSTEM/管理员/交互用户）、固定 16 字节帧头（magic/版本/类型/长度/request ID）、载荷上限 4096 字节、读/写超时、未知版本/类型/超长载荷帧级拒绝（不做宽松转换）、记录客户端 PID + 会话；`--ipc-pipe client [suffix]` 客户端——连接后发送一帧 FactsSnapshot（载荷为 CPOPFACTS/1 结构化 UTF-8 事实：信封行 + `key=value` 行，键 ASCII 字母/数字/`_`/`-`、值 0..128 字节且禁控制字符、去重、至多 64 条，任一违反整体拒绝）并校验应答（requestId 配对、Error 应答不伪装成功）；服务端默认处理器解析结构化事实并过 **v1 键语义白名单**（仅接受已注册键 `client_pid`/`memory_total_mb`/`memory_available_mb`/`memory_load_percent`/`user_idle_seconds`/`observer`，数值键须十进制整数且界内、available<=total、至少一条已注册事实，未知键/越界/空整份整体拒绝），合法回 Ack（紧凑摘要，超限截断）、任一违反回 Error(InvalidFacts)；**会话级身份裁决**（受理前按服务端记录的客户端身份裁决，默认仅受理可识别（pid != 0）且位于交互会话（session != 0）的客户端，拒绝回 Error(UnauthorizedClient) 并断开；`Options.clientGate` 可注入自定义裁决或显式放行）；**会话凭据 token**（`Options.expectedToken` / CLI `--ipc-token <t>`，服务端要求 FactsSnapshot 载荷携带完全匹配的 agent_token 事实，缺失/不匹配回 Error(AuthFailed)；凭据属内部项不回显到应答/摘要；明文仅供 demo，真实供给（按用户派生/ACL 注入）属后续切片）；**客户端用户 SID 授权白名单**（服务端经访问令牌只读查询客户端用户 SID——OpenProcess(PROCESS_QUERY_INFORMATION)+OpenProcessToken(TOKEN_QUERY)+GetTokenInformation(TokenUser)+ConvertSidToStringSidW，同用户可读、跨用户不可读为空串；`Options.allowedClientSids`/CLI `--ipc-allow-user <SID>` 非空时要求 SID 大小写不敏感命中，未命中/未知回 Error(UnauthorizedClient)；拒绝路径先回 Error 再“排空读”断开关闭，避免对端读到 233 而非错误码）；**会话凭据 token 真实供给**（`ipc_credentials` 私有 ACL 存储（LOCALAPPDATA）+ `--ipc-credential provision/status` 与 `--ipc-token-file`，token 永不打印、可轮换）；演示客户端上报**真实内存观测**（QueryMemoryStatus 只读：total/available/load）；单请求-应答握手（断开前服务端等待客户端关闭）；**同一连接多帧会话**（`IpcSession::ServeSession`：接受一个客户端后在同一连接上连续服务多帧，每帧独立读帧-严格校验-裁决-处理器-应答，直到客户端关闭、帧间空闲超时（心跳丢失）或会话窗口到期，返回已服务帧数与结束原因；客户端 `IpcRoundTripSession` 连接一次多帧往返，requestId 逐帧配对、任一 Error 应答不伪装成功；CLI 演示 `--ipc-pipe server --session` × `--ipc-pipe client --frames <1..16>`，同一连接多帧无需重连）；**多实例并发受理**（`IpcConcurrentSummary` + `RunConcurrentServer`：以 N 个独立管道实例/线程并发受理并服务多个客户端，每连接按会话语义服务，窗口到期 join 全部 worker 并释放实例，无脱逸线程；CLI `--ipc-pipe server --instances <1..8>`）；Agent 实体仍属后续扩展点

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
CppOptimizer.exe --activity <s> [idle-secs] [--session] [--foreground]  用户输入活动观测（1–60 秒，只读 GetLastInputInfo、无 Hook；空闲阈值 1..3600 秒，默认 15；--session 追加只读会话上下文：remote/local、锁屏（WTSSessionInfoEx）与断开状态，断开会话/锁屏覆盖输入态为 Disconnected/Locked；--foreground 在 Active/Idle 样本上输出前台窗口所属进程 pid，无窗口 none、查询失败 unavailable，Locked/Disconnected/Unknown 不归属）
CppOptimizer.exe --log <module> <message...>  写一条 Info 日志到 stderr（同步，只读）
CppOptimizer.exe --config <path>  解析并校验 TOML 配置文件（只读）
CppOptimizer.exe --cpu           采样 CPU 使用率（只读，PDH）
CppOptimizer.exe --watch <s> [config.toml]  按配置的游戏规则观测进程生命周期（只读，Toolhelp，1–60 秒；规则 window_title_contains 非空时按主窗口标题子串过滤命中进程）
CppOptimizer.exe --list-processes [--all] [filter]  列出运行进程（只读；默认仅有可见窗口者）
CppOptimizer.exe --add-game [pid] [main.toml] [--dry-run]  从运行进程添加游戏规则到 config.local.toml（无 pid 时交互选择）
CppOptimizer.exe --policy <s> [config.toml]  决策窗口（1–60 秒；门禁开启时落地 R1 执行，门禁全关为纯咨询；[config.toml] 的 [policy].user_away_idle_seconds > 0 开启用户在场门禁——无输入达阈值视不在场抑制优化建议）
CppOptimizer.exe --power-lock <s> [execution|display|both] [reason...]  持有电源请求（R1，前台有界 1–60 秒，退出自动释放）
CppOptimizer.exe --priority-boost <s> <pid> [config.toml]  临时提升进程优先级类（R1，前台有界 1–60 秒，条件恢复；等级取自 [priority].max_level）
CppOptimizer.exe --service console <s|run> [config.toml] [--ipc-facts] [--tray] [--confirm-recovery] [--ipc-token <t>]/[--ipc-token-file [<path>]] [--ipc-allow-user <SID>]  服务宿主控制台模式（前台有界 1–60 秒，或 run 常驻直到 Ctrl+C 优雅停止，R0 只读负载；--ipc-facts 时同时作为受保护管道服务端连续受理 Agent 事实；--tray 时在通知区显示图标，右键“退出”优雅停止；[config.toml] 经 [ipc] 节配置 Safe Mode 门禁参数——缺省 3 次/5s/冷却 2s，越界或为零配置直接拒绝；无显式配置时自动消费每用户默认配置 %LOCALAPPDATA%\CppOptimizer\config.local.toml，存在但无效 -> Safe Mode「配置无效」锁存）；--confirm-recovery（IPC-018）确认上次异常退出（清恢复标记）
CppOptimizer.exe --agent run <s> [suffix] [--interval-ms <50..10000>] [--max-interval-ms <n>] [--away-after-seconds <n>] [--away-cap-ms <n>]  周期上报客户端（前台有界 1–60 秒；每周期真实内存事实 + user_idle_seconds 用户活动事实经受保护管道上报，可选 agent_token；连接失败有界重试与退避，连续失败后周期间隔自适应放大至 --max-interval-ms（缺省 max(3×interval,5s)）；--away-after-seconds/--away-cap-ms 开启在场退避（用户空闲达阈值放大至封顶、恢复输入回基础周期，缺省关闭）；窗口结束输出 sent/connect failures 汇总）
CppOptimizer.exe --ipc-pipe server <s> [suffix] [--session] [--instances <1..8>]  受保护命名管道服务端（前台有界 1–60 秒；至多一个客户端，默认一帧——FactsSnapshot 载荷依次过 CPOPFACTS/1 语法解析与 v1 键语义白名单，任一违反回 Error；--session 在同一连接上连续服务多帧直至客户端关闭/帧间空闲 2 秒/窗口到期；--instances n（>1）以 n 个实例/线程并发受理多个客户端）
CppOptimizer.exe --ipc-pipe client [suffix] [--frames <1..16>]  受保护命名管道客户端（上报 CPOPFACTS/1 结构化真实内存事实并打印应答摘要；--frames n 连接一次依次发送 n 帧、无需重连）
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
  request  : FactsSnapshot id=1 payload=130 bytes
  reply    : ack sent (facts parsed and summarized)

> 终端 B：CppOptimizer.exe --ipc-pipe client
IPC pipe client (protected transport)
  pipe     : \\.\pipe\CppOptimizerIpc
  request  : FactsSnapshot id=1 payload=130 bytes (5 facts)
  facts    : facts ok (5): client_pid=13384 memory_total_mb=32394 memory_available_mb=19816 memory_load_percent=38 observer=CppOptimizer ipc demo
  reply    : Ack id=1 payload=132 bytes [facts ok (5): client_pid=13384 memory_total_mb=32394 memory_available_mb=19816 memory_load_percent=38 observer=CppOptimizer ipc demo]
```

```text
> 终端 A：CppOptimizer.exe --ipc-pipe server 8 --session
IPC pipe server (protected transport, multi-frame session, 8 s)
  client   : pid 9596, session 1
  session  : served 3 frame(s) on one connection, end: client closed
  request  : FactsSnapshot id=3 payload=129 bytes
  reply    : ack sent (facts parsed and summarized)

> 终端 B：CppOptimizer.exe --ipc-pipe client --frames 3
IPC pipe client (protected transport)
  session  : 3 frames on one connection (no reconnect)
  request  : FactsSnapshot id=1..3 payload=129 bytes (5 facts each)
  reply    : Ack id=1 payload=131 bytes [facts ok (5): ...]
  reply    : Ack id=2 payload=131 bytes [facts ok (5): ...]
  reply    : Ack id=3 payload=131 bytes [facts ok (5): ...]
```

## 测试

```powershell
ctest --preset test-debug
```

当前覆盖：错误模型与资源所有权、内存快照契约（输入校验、`used` 派生、`available == total` 边界）、字节显示与快照时效边界、观测窗口聚合（空窗口 / 越界错误路径、round-half-up、顺序无关、整数溢出安全）、低负载占比（严格小于语义、阈值 0/100 边界、round-half-up）、结构化日志（级别过滤、格式化纯函数、文件 sink 与 RAII 关闭、失败降级不递归、并发写）、PDH 采样（warming-up、节奏契约）、进程生命周期（名称匹配、规则匹配、状态差分全状态机、PID 重用/重启、窗口/创建时间查询、轮询线程事件投递、window_title_contains 标题子串过滤：同名实例按标题择一/标题未知或无可视窗口不命中/ASCII 大小写不敏感与中文子串/空过滤保持仅进程名匹配、BuildGameRules 标题携带与非法 UTF-8 拒绝）、进程目录（详情查询、窗口过滤、子串匹配）、规则生成与配置写入（id 派生/去重、TOML 转义、原子写、main+local 合并）、宽字符控制台输出（UTF-8 往返）、策略决策（余量计算与分级边界、规则评估全分支含 prio_boost 与 user_away 在场门禁（不在场抑制优化/危急优先/无游戏优先/在后台暂停前拦截）、防抖冷却语义、求值器组合、`[policy]` 配置校验（含 user_away_idle_seconds 默认 0/解析/负值与超限拒绝、halt_after_action_failures 默认 0/解析/负值与超限拒绝）、`[ipc]` Safe Mode 门禁配置（默认/覆盖/enabled=false/越界与零值拒绝））、电源请求（可注入 fake 的引用计数状态机：配对释放、幂等、失败路径、RAII 自动释放、类型解析）、优先级提升（可注入 fake 的租约状态机：最小权限、身份重验、条件恢复不覆盖外部修改、失败不伪装成功、目标退出视为取消、max_level 门禁）、策略执行器（可注入双 fake 的期望状态对账：门禁开关、幂等、目标变化替换、游戏退出自动释放、电源生命周期、失败路径、连续失败停摆（IPC-017：默认关闭/达阈值停摆不再调用后端且返回 skipped/成功复位计数/阈值 1/ResetHalt 恢复）、RAII）、服务宿主（运行模式解析、状态机合法/非法转移、上报构造与控制码、可注入 SCM fake 的服务状态序列、控制码分支、负载失败/上报失败不伪装、控制台生命周期（有界/常驻/零时长拒绝）、托盘宿主（可注入图标后端：启停生命周期/图标添加失败报错/重复启动拒绝/右键退出 WM_COMMAND 路径回调并停止）、在场台账（分类边界：阈值处 Away/未上报 Unknown/阈值 0 不判定；多客户端任一在场即 Present、全 Away 才 Away；遗忘时长清理；`EffectivePresenceAwaySeconds` policy 阈值 0 回退默认；汇总变化事件：仅状态变化触发、重复调用不触发；`AppendPresenceTransitionLine` 时间线文件追加：空路径拒绝/父目录自建/目录当文件如实失败/追加可读）、审计记录（`optimizer::audit`：追加顺序保存/环形丢最旧/disabled 时 Append 如实失败/格式化；PolicyExecutor 动作观察者在每次后端调用后回调——priority.boost/unboost、power.hold/release 带成功与否）、停止幂等粘性、安装/卸载参数校验、真实后端非 SCM 启动失败路径）、受保护命名管道 IPC（帧头构造/严格解析与校验全项、长度边界、序列化往返、可注入双 fake 的单帧会话：Ping/Ack 与 FactsSnapshot 应答、自定义处理器、非法帧/未知版本/未知类型/超长载荷拒绝并回 Error、接受超时、创建/读写失败不伪装、处理器失败回 Error、默认处理器拒绝、客户端往返 requestId 配对与 Error 应答不伪装、连接/读写失败路径；IPC-002 Facts 载荷契约：序列化/解析往返与首行信封、多字节 UTF-8 值往返、信封缺失/版本不符/旧格式/空载荷/尾随换行/空行/CR/缺 '='/空键/非法键字符/重复键/控制字节/键值超限/条数超限整体拒绝、编码同规则拒绝非法输入、摘要格式与超限截断有界、违反契约的 FactsSnapshot 回 Error(InvalidFacts)；IPC-003 键语义白名单：已注册键合法集合/边界接受、未知键（含未注册候选键）/空整份/非十进制值/越界/available>total/空 observer 整体拒绝、user_idle_seconds（ACT-005）合法值接受与超 uint32 溢出整体拒绝、白名单外的 FactsSnapshot 回 Error(InvalidFacts)；IPC-004 会话级身份裁决：默认裁决拒绝会话 0/不可识别 PID 并回 Error(UnauthorizedClient)、自定义裁决可显式放行或按规则拒绝、客户端把 UnauthorizedClient 应答解析为失败且可见原因；IPC-005 会话凭据 token：agent_token schema 规则与摘要不回显、expectedToken 匹配回 Ack/缺失或不匹配回 Error(AuthFailed)、未配置时忽略凭据；IPC-006 用户 SID 授权白名单：匹配/未知/不匹配裁决与大小写不敏感、授权开启但 SID 未知或不在白名单回 Error(UnauthorizedClient)、未配置不启用；IPC-019 foreground_pid 键：已注册数值键 >=1 接受、0/溢出/未知键整体拒绝、混合载荷可用 NumericFactValue 取 pid；IPC-008 同一连接多帧会话：两帧同连接仅一次 accept/断开/关闭且逐帧 Ack requestId 配对、Facts 多帧应答摘要一致、帧间空闲超时（IdleTimeout）与会话预算（SessionBudget）两种正常结束语义、会话中非法第二帧回 Error(InvalidHeader) 并终止、每帧独立凭据校验（缺 token 回 Error(AuthFailed)）、0 帧（连接即关）不伪装成功、空闲/预算参数非正拒绝；客户端多帧会话：连接一次两帧往返、空序列不连接、中途 Error 应答中止且原因可见、中途 requestId 不配对拒绝、连接失败不伪装；IPC-009 多实例并发受理：参数非法（instances=0/窗口或帧间空闲非正/工厂为空）Validation 拒绝且不启动 worker、窗口到期全部 join 且实例释放（有界）、单 worker 成功服务客户端会话并汇总、真实双管道实例并发服务双客户端集成验证；IPC-010 受理门禁与 Safe Mode：verdict 观察者（正常受理回调一次 Accepted、裁决/SID 拒绝回调 UnauthorizedClient、凭据不符回调 AuthFailed 且不再回调 Accepted）、SafeModeGuard 状态机（时间窗口内失败达阈值进入 Safe Mode 暂停受理且正常受理不复位、窗口外失败自然过期、冷却到期自动恢复并清窗防瞬间重入、enabled=false 恒 Normal、冷却剩余时长随时钟推进）；IPC-015 SafeModeGuard 离散异常触发：触发即锁存且远超冷却不自动恢复、ClearAnomaly 显式恢复受理、enabled=false 不生效、锁存期拒绝不累计（清除后需重新累计）、ClearAnomaly 不影响计数冷却路径）；操作系统支持判定（PlatformTests：Win10 1507/1607/19045 与 Win11 22000 x64 受支持、build<10240/Win8.1/Win7/未知或未来 major/minor 非 0 不支持、版本达标但非 x64 判 UnsupportedArchitecture）；IPC-011 周期上报客户端：窗口内多周期全部收到配对应答、宿主离线计入 connect failures 且 0 上报后窗口正常返回、采样失败（requestFactory 失败）为致命且不发起连接、单次上报前 N 次连接失败后按上限重试成功（connect failures 为 0）；IPC-012 自适应上报节奏：连续失败后间隔指数放大至封顶；ACT-007 在场退避：在场/未配置/恰好达阈值、放大与步进边界/封顶/base 超 cap/步进防御、IpcReportNextGap 组合（无失败+在场=base/离场放大/失败更大取失败/离场更大取离场）（1→2×、2→4×、封顶、多次失败仍封顶、base 超 cap 以 cap 为界）、巨大 cap 下不溢出、cap<=0（未配置）退化为固定间隔；NumericFactValue（ACT-006）：通用数值键取值命中、键缺失/空集合/畸形值返回 nullopt、0 与 uint32 上限边界、负数文本拒绝。 ActivityTests（MOD-ACT-001）：tick 差值基本与 32 位回绕（UINT32 边界附近 16ms）、回退按 0 不做负空闲、分类阈值边界（== 阈值即 Idle）、回绕后仍 Active、负阈值防御、状态名往返、观测窗口计数（Active/Idle/Unknown 归类与回调一致、查询失败降级 Unknown）、零样本 Validation 拒绝；ACT-002 会话上下文：Locked/Disconnected 状态名与 SessionLinkState 名往返、WTS 连接值映射（瞬时/未知按 Unknown）、上下文归类优先级（断开优先于锁屏、锁屏覆盖输入、remote 不影响归类）、上下文窗口（正常计数、锁屏/断开覆盖与 idle 透传、会话查询失败整样本 Unknown、输入查询失败 Unknown、零样本拒绝且不查询、混合序列汇总与回调一致）；ACT-003 前台归属：可归属状态边界（仅 Active/Idle）、归属映射（hasWindow -> Pid / 无窗口 -> NoWindow）、前台窗口 Active/Idle 归属（pid/no-window 与回调/后端查询次数一致）、锁屏不可归属不查前台、输入失败不可归属不查前台、可归属但前台查询失败按 Unknown 降级不伪造 pid、会话失败整样本 Unknown 且不查前台、零样本拒绝且不查询任何后端、混合序列（Pid/NoWindow/Locked/查询失败）计数与回调一致且前台仅对可归属样本查询）。SVC-008 事件驱动输入源（RawInputEventSource：WM_INPUT 计数、其它消息忽略不计数、无事件 IdleDuration nullopt、事件后非负、空 hwnd Attach 拒绝、未绑定 Detach 幂等；托盘窗口 messageObserver 自定义消息回调/拦截，WM_INPUT 经观察者转发计数）。

## 项目状态与路线图

**当前阶段**：工程基线与只读观测。

- 已完成：统一错误模型、RAII 资源封装、Native API 只读能力探测、内存只读快照与字节格式化、`--observe` 观测窗口聚合与低负载占比、结构化日志器（同步 sink、级别过滤、降级路径）、配置解析与校验（`--config`，toml++）、PDH 只读采样（`--cpu`）、进程生命周期观测（`--watch`，Toolhelp 轮询 + 窗口检测 + PID/创建时间身份）、进程目录（`--list-processes`，路径/窗口/内存详情）、自选进程添加游戏闭环（`--add-game`，规则自动生成 + `config.local.toml` 原子写 + main/local 合并加载）、PolicyEngine 只读决策（`--policy`，压力分级 + 规则评估 + 防抖，`[policy]` 配置节）、PowerLocker 首切片（`--power-lock`，电源请求引用计数状态机 + 可注入后端 + R1 可逆演示）、PriorityBooster 首切片（`--priority-boost`，租约状态机 + 条件恢复 + 可注入后端 + R1 可逆演示）、PolicyEngine 接入执行器（`--policy` 决策经 `[priority]`/`[power]` 门禁落地 R1 动作：前台游戏提升 + 游戏运行期电源请求，游戏退出自动释放，无配置或门禁全关纯咨询）、PolicyEngine 用户在场门禁（ACT-004：`[policy].user_away_idle_seconds`，无输入达阈值视不在场 -> `NoOp(user_away)` 抑制优化建议，锁屏/断开/查询失败均不在场，0=关闭零回归）、用户活动观测接入 Agent 周期上报（ACT-005：`user_idle_seconds` 事实 + v1 键语义白名单注册 + 宿主受理回显，GetLastInputInfo 只读、查询失败省略不伪装）、ServiceHost 首切片（`--service console/install/uninstall` + SCM 入口：控制台/服务双模式宿主、SCM 状态机与安装卸载、可注入后端、R0 只读负载）、UserActivityDetector（`--activity`，MOD-ACT-001：GetLastInputInfo 只读输入空闲观测，Active/Idle/Unknown + 回绕与阈值边界）、UserActivityDetector 会话上下文（ACT-002：`--activity --session` 只读 WTS 探测 remote/锁屏/会话连接状态，Disconnected/Locked 覆盖输入态，失败按 Unknown 降级）、UserActivityDetector 前台窗口归属（ACT-003：`--activity --foreground` 只读 GetForegroundWindow/GetWindowThreadProcessId，仅 Active/Idle 输出前台所属 pid，Locked/Disconnected/Unknown 不可归属不伪造，查询失败按 unavailable 降级）；
- 规划中：Per-user Agent 宿主形态（常驻/托盘/跨会话服务编排）。周期上报客户端实体已落地（`--agent run`，IPC-011）；ServiceHost 服务宿主、受保护命名管道 IPC（含凭据 token 真实供给）与宿主消费真实 Facts（连续受理多客户端、同一连接多帧会话/连接复用、多实例并发受理、Safe Mode 受理门禁）已落地；Agent 实体与跨会话/特权环境验证属后续扩展点；
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
