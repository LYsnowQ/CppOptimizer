# Contributing to CppOptimizer

## 开发前必读

1. `docs/20-user-native-kernel-boundary-learning-guide.md`；
2. `docs/21-formal-engineering-handbook.md`；
3. `docs/22-error-resource-concurrency-guide.md`；
4. `docs/23-dangerous-operation-policy-and-threat-model.md`；
5. 对应模块的设计和 include/source 学习文档。

## 工作流

- 从可构建的 `main` 创建功能分支；
- 一次提交只处理一个逻辑问题；
- 先写/更新契约和测试，再接入真实系统动作；
- 新 Win32 API 必须记录错误域和资源配对；
- 新 Experimental 功能默认关闭；
- 不把 `.user`、构建产物、私有日志、dump 或本机路径提交到仓库。

## PR 必填项

- 变更目的和非目标；
- 风险等级 R0～R4；
- 权限与支持矩阵；
- 资源所有权和线程模型；
- 失败/取消/停止/恢复路径；
- 测试命令和结果；
- 对性能和安全边界的影响；
- 文档是否同步。

## 禁止提交

- 驱动签名绕过、内核 Hook、反作弊绕过；
- `PROCESS_ALL_ACCESS` 等无理由大权限；
- 裸拥有型 HANDLE 泄漏到 DTO；
- 默认开启系统级实验；
- 关闭错误检查来获得“性能”；
- 无来源、无版本、无许可证的第三方代码。

## 注释

注释解释不变量、API 契约和风险，不逐行翻译代码。教学型长解释留在 `.md`；`.hpp/.cpp` 保留正式、适量的关键注释。
