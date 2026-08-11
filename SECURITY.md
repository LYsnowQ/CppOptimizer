# Security Policy

## Supported scope

项目当前仅支持 x64 用户态程序。未发布任何内核驱动。Experimental 功能不视为稳定能力。

## Reporting a vulnerability

请私下报告以下问题，不要先公开可利用细节：

- 高权限服务 IPC 身份验证或 ACL 绕过；
- 任意文件写入、配置注入、路径替换；
- PID 重用导致错误进程被操作；
- 权限提升或 UAC 绕过；
- 危险操作绕过编译/配置/命令行门禁；
- 崩溃后无法恢复全局系统状态；
- 内存破坏、句柄混淆、长度/整数溢出；
- 日志泄露 token、隐私路径或用户输入。

报告应包含：版本/commit、Windows build、运行权限、最小复现、预期和实际行为、日志/调用栈，以及是否执行过系统级动作。

## Security guarantees

项目承诺的设计边界：不注入、不 Hook、不读写游戏内存、不绕过驱动签名/PatchGuard/HVCI/反作弊。项目不保证第三方安全或反作弊软件永不误报。

## Dangerous experiments

Native 内存写、GPU 合成负载和全局电源计划等功能必须默认关闭。研究者应使用 VM/专用测试机、快照、显式确认和恢复流程。不要在重要生产设备首次测试。
