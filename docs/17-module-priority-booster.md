# 模块设计文档：进程优先级提升器（PriorityBooster）

> **所属层**：Layer 3 应急响应层  
> **模块ID**：MOD-PRI-001  
> **状态**：Baseline  
> **学习协作建议**：默认 L1/L2。AI 主导最小权限、PID + 创建时间、租约和条件恢复；学习者实现查询、AboveNormal 局部调用、fake 和目标退出测试。绿色区是 fake/审计，黄色不变量是禁止覆盖外部变更、恢复前重验身份和默认最高 AboveNormal；红色区是 REALTIME、便利性 `PROCESS_ALL_ACCESS` 和未经确认的任意进程调整。

## 职责

- 为已确认的目标进程临时提升优先级类；
- 保存原状态并通过租约/引用计数恢复；
- 处理多游戏、进程退出、PID 重用和外部修改冲突。

## 安全边界

- 禁止 `REALTIME_PRIORITY_CLASS`；
- 默认最高 AboveNormal，High 仅显式配置；
- 不逐线程提升、不关闭 priority boost、不设置 affinity；
- 只请求 `PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION | SYNCHRONIZE`；
- 不读写进程内存。

## 恢复语义

进程由 PID + 创建时间/generation 标识。最后租约释放时，仅当进程仍是同一实例且当前值仍等于模块设置值时恢复，避免覆盖用户或第三方工具的修改。

## 验收

多租约、权限拒绝、快速退出、PID 重用、外部改值均正确；Process Explorer 可观察提升与恢复。
