# Contributing to CppOptimizer

> 本文件是**唯一的公开协作规范**。私有设计/工程文档（本地 `docs/`，不随仓库分发）只做补充。
>
> **规则标注**：每条规则前标注适用模式，未标注即对所有模式生效。
> - `[通用]` 单人/多人**都生效**，不得因模式放宽。
> - `[单人]` 只在单人维护模式生效。
> - `[多人]` 只在多人协作模式生效；**规则内容完整保留**，切换后立即生效。

## 0. 模式声明与切换

### 0.1 当前模式

| 项 | 值 |
|---|---|
| **当前模式** | **单人维护（solo）** |
| 决定日期 | 2026-10-24（所有者） |
| 生效范围 | 分支、审查、发布、runner |

### 0.2 随时可切换

模式是**一个开关**，不是重写规范。切换时只改本表 + `docs/design/guides/33-git-workflow-adaptation.md` 的记录：

| 从 → 到 | 需要执行 |
|---|---|
| 单人 → 多人 | ① 本表改 `multi`；② GitHub 开分支保护（PR + 1 次审查 + `static` 检查通过）；③ 宣布 `task/*` 为强制路径 |
| 多人 → 单人 | ① 本表改 `solo`；② 关闭分支保护的必需审查（可保留必需检查）；③ 保留已有 issue/分支清理规则 |

**切换不删除任何规则**：`[多人]` 规则始终在文件里，只是当前不判罚。

## 1. 开始前（AI 与人都适用）`[通用]`

1. **先确认分支**：`git branch --show-current`。不在预期分支上就停止并说明，不要用 `git checkout .` / `reset --hard` 自行纠正。
2. 读 [README](README.md) 与相关头文件（`include/**` 注释是契约真相）。
3. 看 `git status --short` 与 `git log -5 --oneline`；不得覆盖他人未提交的修改。
4. AI 会话必须先读本文件与 `.github/pull_request_template.md`，再动代码。

## 2. Issue 规则 `[通用]`

- **先有 issue，再有分支**：`task/<issue>/<slug>` 里的 `<issue>` 必须真实存在。
- 标题：`[bug] 简述` / `[feat] 简述` / `[task] 简述`；一句话说清对象与现象。
- 标签：至少一个类型标签（`bug` / `enhancement` / `docs` / `tooling`）+ 一个风险标签（`R1`～`R3`）。
- 一个 issue 只对应一件事；范围变大就拆新 issue，不把原 issue 改写成大杂烩。
- 关闭：由 PR 正文 `Closes #n` 自动关闭。`[多人]` 合并后核对 issue 已 CLOSED、远端分支已删；工作流失败时合并人当场手工关并留记录。
- `[单人]` 允许省略 issue 直接改 master；但一旦开了 issue，就必须按上面的关闭规则收尾。

## 3. 分支模型

长期分支只有 `master`（正式稳定）。

| 分支 | 用途 | 模式 |
|---|---|---|
| `master` | 正式稳定；`[单人]` 可直接提交；`[多人]` **只能由 PR merge commit 写入** | 通用（限制不同） |
| `task/<issue>/<slug>` | 一次任务一条；合并后立即删除 | 通用（`[单人]` 可选，`[多人]` 强制） |
| `dev/<username>` | 个人自由区；不得作为进入 `master` 的凭据 | 通用 |
| `hotfix/<slug>` | 紧急修复；事后补 issue | 通用 |
| `stage` | 预发布集成分支；`master` 只从 `stage` 快进 | `[多人]`（需要灰度环境时） |

命名：**只用 `/` 分层，段内用 `_`，禁止 `-`**。

```text
task/<issue>/<slug>   slug = [a-z0-9]+(_[a-z0-9]+)*   例 task/42/power_scheme
dev/<username>        username 中 `-` 写成 `_`          例 dev/ly_snowq
hotfix/<slug>
```

- `[通用]` 禁止 force-push `master`；禁止残留已合并的 task 分支。
- `[单人]` 直推 `master` 允许，但门禁（§5）一条不减。
- `[多人]` 禁止直接 push `master`；`task/**`、`dev/**` 不得直接进 `master`；合并只用 **merge commit**（不用 squash/rebase，保留审查与归档引用的 SHA）。

## 4. 提交规范（Conventional Commits）`[通用]`

```text
<type>(<scope>): <中文简述>

为什么改；改变了什么；用什么命令验证。
```

- 首行 ≤ 72 字符，半角冒号，不加句号与 emoji；代码标识符与路径保持英文。
- `type`：`feat` `fix` `refactor` `perf` `docs` `test` `build` `ci` `chore` `style`。
- `scope`：`power` `memory` `policy` `priority` `ipc` `service` `activity` `metrics` `process` `config` `logger` `platform` `audit` `build` `docs` `tooling` `repo`。
- 一次提交一个**可独立回滚**的逻辑目的；代码与其契约、测试、文档属于同一改动。
- 机械搬迁与行为改变尽量分开，但不得制造无法构建的中间状态。
- 破坏性变更用 `!` 或 `BREAKING CHANGE:` 正文说明影响与迁移。
- 禁止 `update`、`fix bug`、`WIP`、`tmp`；禁止夹带无关改动。
- 提交不等于发版：`feat`/`fix` 不自动升版本、不自动发布。

## 5. 提交前门禁 `[通用]`

```powershell
pwsh -File scripts/verify.ps1        # 全量：三构建 + 全量回归 + 静态检查
pwsh -File scripts/verify.ps1 -Fast  # 快速：提交信息 + 仓库卫生（pre-push 用）
```

1. **三构建 0 warning / 0 error**：CMake Debug、CMake Release、MSVC Debug；
2. **全量回归**：`ctest` 全部套件 PASS；行为变更必须带能区分修复前后的回归测试；
3. **公开面自检**：不出现私有文档路径、内部代号、教学用语；
4. **编码规范**：含中文源文件必须带 UTF-8 BOM；`.cmd/.bat` 必须纯 ASCII（cmd 按 OEM 代码页解析）；
5. **仓库卫生**：`git diff --check` 干净、无构建产物/日志/本机路径/凭据。

**R2/R3 额外要求** `[通用]`：除构建与单测外，必须在**隔离来宾**中真实跑通并留证
（门禁 → 执行 → 前后读数 → 审计/日记/冷却）；**来宾未通过不算完成**。

失败、跳过、未验证项必须如实写进提交说明或 PR，不能只截取成功输出。

## 6. PR 规则

- `[单人]` 可选：较大改动、需要留档讨论时用；小改动可直推 `master`。
- `[多人]` **强制**：所有进入 `master` 的改动都必须经由 PR，正文按 [.github/pull_request_template.md](.github/pull_request_template.md) 填写，
  且必须写 `Closes #<issue>`。

PR 正文必填段落（模板逐字保留）：目的、关联、变更范围、风险与权限、解决链路、验证命令与结果、验收证据、人工验收步骤、审查结论、风险与回滚。

## 7. 审查 `[多人]`（单人=自审）

### 7.1 触发

- `[多人]` 任何合入 `master`（或 `stage`）的 PR，含文档、脚本、配置；
- `[多人]` 任何触及认证、授权、凭据、配置契约、危险能力开关的改动；
- `[单人]` 不要求他人审查，但**必须自审**：按 7.2 逐项过一遍，PR/提交说明写「自审」。

### 7.2 逐项清单（单人自审 / 多人互审，同一份）

1. **分支**：分支名合规；`[多人]` 目标分支必须是 `master`（或 `stage`），来源必须是 `task/*`/`hotfix/*`。
2. **密钥**：diff 中不得出现 token、密码、私钥、会话密钥真值；`grep` 通过不等于没泄漏，人眼过新增字符串。
3. **错误域与资源**：新 Win32/Native 调用是否检查返回值、映射到 `common::Error`；HANDLE/token/租约是否成对释放；失败路径是否如实。
4. **门禁**：危险能力是否 编译期开关 + 配置门 + 命令行确认 + 环境门 + 审计 + 冷却 逐项具备，默认是否 OFF。
5. **可逆性**：是否有回滚路径，回滚是否与正向同等验证；不可逆动作是否限隔离环境。
6. **并发与生命周期**：线程模型、`noexcept` 析构、幂等 Stop、部分初始化失败是否只释放已获资源。
7. **测试**：是否新增/更新回归，能否区分修复前后；`[多人]` 是否覆盖失败注入。
8. **来宾证据** `[R2/R3]`：是否有门禁 JSON、前后读数、审计/日记/冷却证据，编号可追溯。
9. **文档同步**：README/头文件注释/规范是否随之更新；不能只改日期冒充同步。
10. **提交信息**：符合 §4；一次提交一个目的。
11. **残留**：不得把旧的模型/开关/路径当成现行规范；历史说明须显式标注「历史」。
12. **清理** `[多人]`：合并后 PR 关闭的 issue 已 CLOSED、远端 task 分支已删、本地 worktree 已清。

### 7.3 结论格式

```text
[严重度] 文件:行 — 理由（指向规则或真实后果）— 修法
严重度：阻塞 / 应修 / 建议
结论只允许：**阻塞** / **有条件通过** / **通过**
```

不得使用「基本没问题」「看起来可以」这类模糊结论，也不得用机器 PASS 代替人对 diff 的判断。

## 8. 合并 `[通用]`

- `[单人]` 直接提交到 `master`，或合并自己的 task 分支（merge commit 或快进均可）。
- `[多人]` 只用 **merge commit**；合并后立即删除远端 task 分支。
- 合并前门禁必须全绿；合并后如需回滚，用 `git revert -m 1 <merge-sha>`，不改写历史。

## 9. 发布

- `[通用]` 发布只靠打 tag（`vX.Y.Z`），需要所有者授权；合并进 `master` 本身不部署。
- `[单人]` 不要求 rc 预发布。
- `[多人]` / `[外部分发]` `vX.Y.Z-rc.N` 先发预发布 → 人工验收 → 在同一提交打 `vX.Y.Z`；
  tag 必须落在对应分支的提交上，版本号与产物一致；禁止移动或删除已发布 tag。
- `[通用]` 不因 `feat`/`fix` 自动升号，版本变更走单独提交。

## 10. 代码与安全边界 `[通用]`

- 先写/更新契约和测试，再接入真实系统动作；真实调用必须可注入 fake 后端。
- 新增 Win32/Native API：记录错误域、资源配对、失败路径；禁止忽略返回值。
- 危险能力默认关闭：编译期开关 + 配置门 + 命令行确认 + 环境门 + 审计 + 冷却，逐项缺一不可。
- 新 Experimental 功能默认 OFF；S3/S4 一类不可逆动作只在隔离环境执行。
- 第三方代码必须有来源、版本与许可证；`thirdParty/` 不入库。

## 11. 禁止提交 `[通用]`

- 驱动签名绕过、内核 Hook、反作弊绕过；
- `PROCESS_ALL_ACCESS` 等无理由大权限；
- 裸拥有型 HANDLE 泄漏到 DTO；
- 默认开启系统级实验；
- 关闭错误检查来获得“性能”；
- 无来源、无版本、无许可证的第三方代码；
- 私有 `docs/`、`works/`、`AI_test/`、`ISOLATION-VM-SETUP.md`、`.orca/`（见 `.gitignore`）；
- 任何真实凭据（token/密码/私钥）——发现即视为阻塞，需轮换。

## 12. 文档同步 `[通用]`

**公开面红线**：README/CONTRIBUTING/SECURITY/源码注释只能引用**已入库**的内容；不得出现 `docs/`、`works/`、`AI_test/`、`ISOLATION-VM-SETUP.md`、`work.md` 等未上传路径。`scripts/verify.ps1` 会拦截。

- 模块行为变化 → 同步 README 特性条目与相关头文件注释；
- 命令/开关/字段变化 → 同步 README 与 `--help`；
- 规范变化 → 本文件 + PR 模板 + 钩子/脚本必须一起改（避免“文档说的”和“钩子做的”不一致）；
- `[多人]` 文档索引（如有）需重新生成并通过检查。

## 13. 注释 `[通用]`

注释解释**不变量、API 契约与风险**，不逐行翻译代码；设计长文留在本地私有文档。

## 14. 本地钩子 `[通用]`

```powershell
git config core.hooksPath .githooks   # 每台克隆一次
```

- `commit-msg`：Conventional Commits 格式与 scope；
- `pre-commit`：拒绝构建产物、私有目录、二进制、>5 MiB 文件；
- `pre-push`：跑 `scripts/verify.ps1 -Fast`，失败拒绝推送。

跳过单次检查用 `--no-verify`；**CI 与人工门禁不因此豁免**。

## 15. CI 与 Runner

- `[通用]` **静态门禁**：GitHub 托管 runner 跑 `scripts/verify.ps1 -Fast -Ci`。
- `[通用]` **构建门禁**：当前在**本机**执行 `pwsh -File scripts/verify.ps1`。
- `[多人]` 或需要 GitHub 侧构建时：注册自托管 runner（`scripts/setup-runner.ps1`，标签含 `cppoptimizer`，`workflow_dispatch` 手动触发）。
  原因：`thirdParty/` 不入库，托管 runner 无法构建。
- `[多人]` 分支保护与必需检查此时开启；`[单人]` 不启用。
