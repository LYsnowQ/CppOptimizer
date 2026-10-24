# Contributing to CppOptimizer

> 统一入口、最小变更、可审查的提交与资源隔离。本文件是**唯一的公开协作规范**；
> 私有设计/工程文档（本地 `docs/`，不随仓库分发）只做补充，不覆盖本文件。

## 0. 开始前

1. **先确认分支**：`git branch --show-current`。预期在 `task/<issue>/<slug>` 或 `dev/<username>` 上；不在就停下来说明，不要靠 `git checkout .` 或 `reset --hard` 自行纠正。
2. 读 [README](README.md) 与相关头文件（`include/**` 的注释是契约真相）。
3. 看 `git status --short` 与 `git log -5 --oneline`；**不得覆盖别人未提交的修改**。

## 1. 分支模型

**长期分支只有 `master`**（正式稳定）。当前为**单人维护**模式：

| 分支 | 来源 → 去向 | 规则 |
|---|---|---|
| `master` | — | 默认可直接提交，但**必须先通过 `scripts/verify.ps1`**；提交信息必须合规 |
| `task/<issue>/<slug>` | 从最新 `master` 拉出 → 合并回 `master` | **推荐**用于较大改动/需要隔离的实验；一次任务一条，合并后删除 |
| `dev/<username>` | 个人自由区 | 可以随意提交；不作为进入 `master` 的凭据，也不参与发布 |
| `hotfix/<slug>` | 线上急修 | 紧急情况下使用，事后补 issue |

命名规则：**只用 `/` 分层，段内不用 `-`**。

```text
task/<issue>/<slug>      slug = [a-z0-9]+(_[a-z0-9]+)*     例 task/42/power_scheme
dev/<username>           username 里 `-` 写成 `_`            例 dev/ly_snowq
hotfix/<slug>
```

- 用 `task/*` 时，`<issue>` 是纯数字 issue 编号；不开 issue 就不用 task 分支（直接改 `master` 也可以，但门禁不打折）。
- 合并 `task/*` 用 **merge commit**，不用 squash / rebase：归档引用的提交 SHA 必须留在历史里。
- 禁止 force-push `master`；禁止残留已合并的 task 分支。

### 1.1 多人协作模式（**当前不启用**）

以下规则已经写进仓库，但**只在进入多人协作时生效**。届时由所有者一次性开启：

| 规则 | 启用条件 | 现状 |
|---|---|---|
| 禁止直接 push `master`，必须走 PR | 出现第二位长期贡献者 | 未启用（单人可直接提交） |
| PR 需至少一次代码审查 + `static` 检查通过 | 同上 | 未启用 |
| GitHub 分支保护（required checks / 禁止 force-push） | 同上 | 未配置 |
| 引入 `stage` 预发布分支（`master` ← `stage` ← `task/*`） | 需要灰度/预发布环境时 | 未创建 |
| 发布走 `vX.Y.Z-rc.N` → 验收 → `vX.Y.Z` 双 tag | 有外部分发需求时 | 未启用（只有 `vX.Y.Z`） |

启用方式与理由见 `docs/design/guides/33-git-workflow-adaptation.md`；在此之前，单人模式的门禁（构建、回归、静态检查、来宾验收）**一条都不减**。

## 2. 提交规范（Conventional Commits）

```text
<type>(<scope>): <中文简述>

为什么改；改变了什么；用什么命令验证。
```

- 首行 ≤ 72 字符，半角冒号，不加句号与 emoji；代码标识符与路径保持英文。
- `type`：`feat` `fix` `refactor` `perf` `docs` `test` `build` `ci` `chore` `style`。
- `scope`（按职责中心选，不罗列文件）：

  | scope | 范围 |
  |---|---|
  | `power` `memory` `policy` `priority` | 对应 `include/source` 模块 |
  | `ipc` `service` `activity` `metrics` `process` `config` `logger` `platform` `audit` | 同上 |
  | `build` | CMake / vcxproj / presets / 编译期开关 |
  | `docs` | README / CONTRIBUTING / 注释性文档 |
  | `tooling` | `scripts/`、`.githooks/`、CI |
  | `repo` | 仓库卫生（行尾、忽略规则、属性） |

- 一次提交一个**可独立回滚**的逻辑目的；代码与其契约、测试、文档属于同一改动。
- 机械搬迁与行为改变尽量分开，但**不得**制造无法构建的中间状态。
- 兼容性破坏用 `!` 或 `BREAKING CHANGE:` 正文说明影响与迁移。
- 禁止 `update`、`fix bug`、`WIP`、`tmp` 等无信息量消息；禁止夹带无关改动。
- 提交不等于发版：`feat`/`fix` 不自动升版本、不自动发布。

## 3. 提交前门禁

在仓库根执行：

```powershell
pwsh -File scripts/verify.ps1            # 全量：三构建 + 全量回归 + 静态检查
pwsh -File scripts/verify.ps1 -Fast      # 快速：提交信息 + 仓库卫生（pre-push 用）
```

全量门禁包含：

1. **三构建 0 warning / 0 error**：CMake Debug、CMake Release、MSVC Debug（`AI_test\build64*.bat`）；
2. **全量回归**：`ctest` 全部套件 PASS；
3. **公开面自检**：仓库内不出现私有文档路径、内部代号、学习/教学用语；
4. **编码规范**：含中文的源文件必须带 UTF-8 BOM；`.cmd/.bat` 必须纯 ASCII（cmd 按 OEM 代码页解析）；
5. **仓库卫生**：`git diff --check` 干净、无构建产物/日志/本机路径。

失败或未验证项必须如实写进 PR，不能只截取成功输出。

**R2/R3 功能额外要求**：除构建与单测外，必须在**隔离来宾**中真实跑通并留证（门禁 → 执行 → 读数 → 审计/日记/冷却），
证据落到本人实验室目录；**来宾未通过不算完成**。

## 4. PR 说明（使用 PR 时）

单人维护时可直推 `master`；一旦使用 PR（较大改动、或多人协作模式），按
[.github/pull_request_template.md](.github/pull_request_template.md) 逐段填写：

- **目的**与**非目标**；关联 issue（`Closes #n`）；
- **风险等级** R0～R4 与权限/支持矩阵；
- **资源所有权与线程模型**（HANDLE/token/租约的配对与释放）；
- **失败/取消/停止/恢复路径**；
- **变更范围**（`power`、`memory`、`ipc`… 各自改了什么、**不做什么**）；
- **验证命令与真实结果**（含来宾证据编号）；
- **风险与回滚**（revert 合并提交 / 恢复快照）。

**审查结论**：单人维护时写「自审」即可（自审也要逐项过门禁）；**要求他人审查属于多人协作模式**（见 §1.1），当前不启用。

## 5. 代码与安全边界

- 先写/更新契约和测试，再接入真实系统动作；真实调用必须可注入 fake 后端。
- 新增 Win32/Native API：记录错误域、资源配对、失败路径；禁止忽略返回值。
- 危险能力**默认关闭**：编译期开关 + 配置门 + 命令行确认 + 环境门 + 审计 + 冷却，逐项缺一不可。
- 新 Experimental 功能默认 OFF；S3/S4 一类不可逆动作只在隔离环境执行。
- 不把 `.user`、构建产物、私有日志、dump、本机绝对路径提交到仓库。
- 第三方代码必须有来源、版本与许可证；`thirdParty/` 不入库。

## 6. 禁止提交

- 驱动签名绕过、内核 Hook、反作弊绕过；
- `PROCESS_ALL_ACCESS` 等无理由大权限；
- 裸拥有型 HANDLE 泄漏到 DTO；
- 默认开启系统级实验；
- 关闭错误检查来获得“性能”；
- 无来源、无版本、无许可证的第三方代码；
- 私有 `docs/`、`works/`、`AI_test/`、`ISOLATION-VM-SETUP.md`（见 `.gitignore`）。

## 7. 注释

注释解释**不变量、API 契约与风险**，不逐行翻译代码；设计长文留在本地私有文档，`.hpp/.cpp` 只保留正式、适量的关键注释。

## 8. 本地钩子（每台克隆做一次）

```powershell
git config core.hooksPath .githooks
```

- `commit-msg`：校验 Conventional Commits 格式与 scope；
- `pre-commit`：拒绝把构建产物、私有目录、二进制带进暂存区；
- `pre-push`：跑 `scripts/verify.ps1 -Fast`（提交信息 + 卫生），失败即拒绝推送。

临时跳过单次检查用 `git commit --no-verify` / `git push --no-verify`；**CI 与人工门禁不会因此豁免**。

## 9. CI 与 Runner

- **静态门禁**：GitHub 托管 runner（`ubuntu-latest`，自带 `pwsh`）执行 `scripts/verify.ps1 -Fast -Ci`。
- **构建门禁**：**当前在本机执行**（`pwsh -File scripts/verify.ps1`），不注册自托管 runner。
  需要 GitHub 侧构建时再启用：`scripts/setup-runner.ps1`（token 只在页面取用，不入库），
  标签须含 `cppoptimizer`，`workflow_dispatch` 手动触发。
- 不启用 runner 不影响任何门禁：本地脚本就是同一套实现。
- 单人维护时**不启用**分支保护与必需检查；进入多人协作模式后再开启（见 §1.1）。

## 10. 发布

发布只靠打 tag（`vX.Y.Z`），由所有者授权后执行；合并进 `master` 本身不产生发布。
双 tag（`-rc.N` → 验收 → 正式）属于多人协作/外部分发阶段，当前不启用。
