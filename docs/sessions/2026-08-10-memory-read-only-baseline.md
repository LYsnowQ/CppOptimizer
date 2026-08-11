# MemoryTuner 第一轮会话记录：只读内存状态基线

## 1. 会话元信息

- 日期：2026-08-10
- 模块：MemoryTuner
- 目标：建立只读物理内存状态构造
- 使用环境和工具链：当前环境辅助 MinGW；正式工具链 MSVC 尚待验证
- 本次是否允许修改代码：是
- 本次范围：R0 一次性查询、纯逻辑构造、测试和文档

## 2. 开始前事实检查

- 已阅读：docs/00、02、07、13、14、20～26；
- 已检查：Common Error、NativeApi、构建配置和现有测试；
- 冲突：旧 docs/07 仍展示已废弃的清理等级和 Native 写架构；真实 MemoryTuner 代码已在上轮审查中移除。

## 3. 任务卡

- 技术层级：用户态 Win32
- 风险等级：R0
- 教学级别：L2
- 推荐 API：`GlobalMemoryStatusEx`
- AI 负责：接口、不变量、API 调用、安全边界、项目接线和基础测试
- 学习者负责：新增一个边界测试、执行调试观察并解释 API
- 绿色自由区：纯函数、只读查询、格式化、测试
- 黄色不变量：错误域、字节单位、失败不伪装成零、立即保存 GetLastError
- 红色禁止区：Native 写、列表清理、工作集调整、权限提升、周期任务
- 基础验收：严格编译；纯逻辑测试；人工观察一次真实查询
- 安全变体：测试 available 等于 total 或 load 等于 100

## 4. API 卡片摘要

### GlobalMemoryStatusEx

- 使用原因：公开 Win32 物理内存状态查询；
- 头文件 / 库：Windows SDK / Kernel32；
- 成功 / 失败：非零 / 零；
- 错误域：Win32，失败后立即读取 GetLastError；
- 最小权限：标准用户；
- 所有权：无返回资源；
- 副作用：无。

## 5. 实际完成

- 新建 `include/memory/memory_tuner.hpp`；
- 新建 `source/memory/memory_tuner.cpp`；
- 新建 `tests/memory_tests.cpp`；
- 建立 include/source 学习说明并重写过期的 docs/07；
- 接入 CMake 和 Visual Studio 工程；
- 增加纯逻辑正常/错误测试和一次真实 R0 查询测试；
- 搜索确认 MemoryTuner 真实代码没有 Native 写、列表清理、工作集调整或权限提升；
- 学习者额外边界测试和实际调试观察尚待完成。

## 6. 验证证据

```text
Built：MinGW 辅助严格编译通过；主程序和 MemoryTests 均使用 -Wall -Wextra -Wpedantic -Werror
Tested：未执行；当前 Bash 环境无法直接运行 MinGW 生成的 Windows 可执行文件，返回 127
Observed：尚未在 Visual Studio 调试器观察 GlobalMemoryStatusEx
Not verified：MSVC x64 Debug/Release、CTest、真实 MemoryTests 执行、人工数值对照
```

## 7. 下一次最小任务

学习者独立增加一个合法边界测试（建议 `available == total`），并在 Visual Studio 中单步观察 `GlobalMemoryStatusEx` 成功路径；不扩大到任何清理接口。
