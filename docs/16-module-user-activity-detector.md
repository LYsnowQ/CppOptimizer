# 模块设计文档：用户活动检测器（UserActivityDetector）

> **所属层**：Layer 1 监控感知层  
> **模块ID**：MOD-ACT-001  
> **状态**：Baseline（仅 Per-user Agent）  
> **学习协作建议**：默认 L1/L2。AI 提供 Session、`GetLastInputInfo`、窗口归属和服务隔离的 API 卡片；学习者实现只读快照、Idle 计算和状态测试。绿色区是阈值与纯计算，黄色不变量是不记录输入内容、Agent/Service 边界和时间回绕处理；红色区是全局输入 Hook、键鼠内容采集和 Session 0 直接检测交互用户。

## 职责

- 读取最后输入时间并判断 Active/Idle；
- 跟踪锁屏、解锁、会话连接/断开；
- 提供前台窗口所属 PID 和远程会话标志；
- 向 PolicyEngine 提供只读活动快照。

## 安全边界

不安装全局 Hook，不记录按键、鼠标位置或输入内容。服务 Session 0 不直接运行本模块，由交互用户 Agent 采集后通过受保护 IPC 上报。

## API 后端

`GetLastInputInfo`、`GetForegroundWindow`、`GetWindowThreadProcessId`、`WTSRegisterSessionNotification`、`GetSystemMetrics(SM_REMOTESESSION)`。

## 状态

`Unknown / Active / Idle / Locked / Disconnected`。阈值来自配置，状态变化需防抖，系统唤醒后先进入 Unknown 再重新采样。

## 验收

键鼠活动和 Idle 阈值正确；锁屏/RDP 正确；无管理员权限；无输入隐私日志；空闲开销可忽略。
