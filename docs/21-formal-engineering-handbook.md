# CppOptimizer 正式工程开发手册

> **状态**：Engineering Baseline v1.0  
> **适用范围**：文档、用户态程序、测试、未来可选驱动  
> **协作方式**：AI 与学习者的任务分工、教学梯度和跨会话协议见 [00-project-ai-learning-harness.md](00-project-ai-learning-harness.md)。该协议不能降低本手册的正式工程要求。

---

## 1. 工程目标

本项目虽然用于学习，但按正式软件工程管理：可构建、可测试、可审计、可回滚、可发布。学习注释解释“为什么”，生产代码保持适量注释，不逐行翻译语法。

优先级顺序：

```text
系统安全 > 数据正确 > 可恢复 > 可观测 > 性能收益 > 开发便利
```

---

## 2. 支持矩阵

| 项目 | 正式支持 | 说明 |
|---|---|---|
| OS | Windows 10 22H2、Windows 11 当前受支持版本 | 其他版本尽力而为 |
| 架构 | x64 | Win32 仅教学编译，不发布 |
| 编译器 | MSVC / Visual Studio 2022 | C++20 |
| 字符集 | Unicode | 内部 UTF-16 路径、UTF-8 日志/配置 |
| 默认权限 | 标准用户 | 管理员仅用于明确实验 |
| 驱动 | 不包含 | 独立阶段评审后决定 |

---

## 3. 分层依赖规则

```text
app/service
    ↓
policy
    ↓
metrics/process/activity + executors
    ↓
config/logger/common/platform
```

约束：

- `common` 不依赖业务模块；
- Logger 不依赖 ConfigManager；
- ConfigManager 可使用 Common，但初始化早期错误需有降级输出；
- PolicyEngine 不直接调用 Win32 API；
- 未文档化 Native API 只能出现在 `platform` 适配层；
- 模块间不共享可写全局变量；
- include 中不暴露第三方库类型，必要时使用 PImpl。

---

## 4. C++ 编码规范

### 4.1 命名

- 类型：`PascalCase`；
- 函数：`PascalCase`（保持当前项目风格）；
- 局部变量/参数：`camelCase`；
- 私有成员：`camelCase_`；
- 常量：`kPascalCase`；
- 宏只用于平台预处理，使用 `UPPER_SNAKE_CASE`。

### 4.2 类型

- 字节数、计数：`std::uint64_t`；
- PID/TID、Win32 flag：使用 SDK 类型 `DWORD`；
- 时间间隔：`std::chrono`；
- 路径：`std::filesystem::path`；
- 不使用 `long` 表达固定宽度数据；
- 不以 `int` 表示 bool/枚举；
- 失败和值为 0 有歧义时使用 `Result<T>` / `std::optional<T>`。

### 4.3 所有权

- 裸指针默认非拥有；
- Win32 句柄必须进入对应 traits 的 unique resource；
- `SC_HANDLE` 使用 `CloseServiceHandle`，不能使用 `CloseHandle`；
- COM 使用 `ComPtr`；
- `LocalAlloc/FormatMessage` 使用 `LocalFree`；
- `CoTaskMemAlloc/SHGetKnownFolderPath` 使用 `CoTaskMemFree`；
- 不在可复制 DTO 中放拥有型 HANDLE。

### 4.4 异常

- 模块边界不允许异常逃逸到 Win32/SCM/线程回调；
- 普通构造和标准库可使用异常，但顶层捕获并转换为项目错误；
- 析构、`Stop`、`Shutdown` 为 `noexcept`；
- 危险恢复路径不能依赖抛异常。

---

## 5. 注释标准

### 应注释

- API 为什么选择、权限和版本要求；
- 不直观的成功/失败条件；
- 创建/释放配对；
- 并发不变量、锁顺序和线程归属；
- 风险、回滚和刻意不做的行为；
- Native API 来源和兼容假设。

### 不应注释

```cpp
count++; // count 加一       // 无价值
```

推荐：

```cpp
// PDH 速率计数器的第一次采样只有基线，不向策略层发布为有效值。
if (!counter.hasBaseline) { ... }
```

所有 `TODO` 使用：

```text
TODO(owner, issue-id): 原因和完成条件
```

禁止没有责任和完成条件的永久 TODO。

---

## 6. 并发规范

- 每个模块在文档中声明线程模型和 callback 线程；
- 回调永不在内部锁内调用；
- 不持锁执行 Win32 阻塞 API、磁盘 I/O 或用户回调；
- 锁顺序固定并写入注释；
- 停止使用 event/stop_token 唤醒，不以轮询等待退出；
- 线程必须可在限定时间内 join；
- 状态机转换在单一串行上下文发生，或由同一互斥保护；
- 原子变量只用于简单独立状态，不用来拼凑复杂无锁协议。

---

## 7. 构建规范

### Debug

- `/W4 /permissive- /sdl /EHsc /Zc:__cplusplus`；
- 启用运行时检查、符号和静态分析；
- Native 写操作仍默认关闭。

### Release

- 保留 PDB；
- 启用 CFG、DEP、ASLR、SDL、LTCG；
- 不因 Release 移除错误检查或审计；
- 发布只包含 x64。

警告策略：新代码目标零警告。接入 `/WX` 前先清理既有警告，CI 可按目录逐步启用。

---

## 8. 配置和功能开关

每个危险能力同时具有：

1. 编译期开关，例如 `OPTIMIZER_ENABLE_NATIVE_MEMORY_WRITE=0`；
2. 配置开关，默认 false；
3. 命令行显式确认；
4. 运行时权限/环境检查；
5. PolicyEngine 安全门禁；
6. 审计日志。

任一条件不满足即拒绝，不能自动“帮用户开启”。

---

## 9. 分支与提交

- `main` 始终可构建；
- 功能分支：`feature/<module>-<topic>`；
- 修复分支：`fix/<module>-<topic>`；
- 每个提交只表达一个逻辑变化；
- 提交信息：`type(module): summary`；
- 危险功能 PR 必须包含风险、回滚、测试证据和默认状态。

---

## 10. AI 辅助开发的工程约束

AI 生成或补全的代码与人工代码采用相同标准，不存在“教学代码可以忽略错误路径”的例外。

开始实现前必须明确：

- 任务风险 R0～R4；
- 教学完成度 L0～L4；
- AI 与学习者各自负责的部分；
- 绿色自由修改区、黄色不变量和红色禁止区；
- 编译、测试和调试验收方式。

AI 可以完整示范第一次出现的小型通用模式，但不应在 Markdown 长期复制整个 `.cpp`。后续同类任务应逐渐转为骨架补全、契约实现和选型评审。学习者提交的代码默认先采用提示优先审查；只有危险边界、结构不可挽救、连续受阻或学习者明确要求时，才进行局部或整体替换。

任何跨设备或跨 AI 会话的交接都必须区分：已实际验证、仅静态检查、尚未验证以及无法验证的原因。

## 11. Definition of Done

除 docs/14 要求外，正式模块还必须：

- 公开契约和线程模型完整；
- 权限不足/不支持/超时/取消均测试；
- 资源和恢复路径经过审查；
- 日志不泄露敏感信息；
- 标准用户路径可用；
- Experimental 功能默认关闭；
- 有最小性能预算和长稳数据；
- 文档与代码同步更新；
- 学习任务记录了 API 卡片、自由边界和实际验证；
- 至少一个关键函数、测试或安全变体由学习者完成并能解释；
- 跨会话交接说明剩余风险和下一最小任务。
