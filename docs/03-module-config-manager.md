# 模块设计文档：配置管理中心 (ConfigManager)

> **所属层**：跨层基础设施  
> **模块ID**：MOD-CFG-001  
> **状态**：大纲阶段  
> **学习协作建议**：默认 L2，熟悉 schema 和快照后升至 L3。AI 负责接口边界、热重载并发和 parser 适配；学习者主导默认值、校验、匹配、纯函数与测试。绿色区是 schema/校验/格式化，黄色不变量是不可变快照、锁外 callback 和失败时保留最后有效配置；不得用配置自动打开 R2/R3 能力。

---

## 一、模块概述

### 1.1 职责
- 配置文件的解析、校验与内存化
- 运行时配置热重载（无需重启程序）
- 命令行参数覆盖
- 配置变更事件通知

### 1.2 设计原则
- **单一数据源**：所有配置必须来自 ConfigManager，禁止模块私自读文件
- **不可变性快照**：配置读取时获取快照，避免运行时配置变更导致竞态
- **失败安全**：配置错误时使用硬编码默认值，程序不崩溃

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::config {

class ConfigManager {
public:
    static ConfigManager& Instance();
    bool Initialize(const std::wstring& configPath, int argc, wchar_t* argv[]);
    bool Reload();

    std::optional<GameConfig> GetGameConfig(const std::string& gameName) const;
    std::shared_ptr<const Config> GetSnapshot() const;
    ThresholdConfig GetThresholdConfig() const;
    LayerConfig GetLayerConfig() const;
    LoggingConfig GetLoggingConfig() const;

    using ConfigChangeCallback = std::function<void(ConfigSection)>;
    void RegisterCallback(ConfigSection section, ConfigChangeCallback cb);

    bool HasCommandLineFlag(const std::string& flag) const;
    std::string GetCommandLineValue(const std::string& key) const;

private:
    ConfigManager() = default;
};

} // namespace optimizer::config
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│           ConfigManager (单例)           │
├─────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────────┐  │
│  │ 文件监控器   │  │  配置验证器      │  │
│  │(FileWatcher)│  │ (ConfigValidator)│  │
│  └──────┬──────┘  └─────────────────┘  │
│         │                               │
│  ┌──────▼──────┐  ┌─────────────────┐  │
│  │  配置解析器  │  │  配置存储 (原子)  │  │
│  │(TOML Parser)│  │ (Atomic<Config>) │  │
│  └─────────────┘  └─────────────────┘  │
├─────────────────────────────────────────┤
│  事件总线：配置变更 → 回调分发            │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 配置格式选择
- **首选**：TOML（人类可读、有标准、支持注释）
- **备选**：JSON（如需要 Web 配置界面）
- **库**：`toml++` (header-only, C++17)

### 4.2 热重载机制
- 使用 `ReadDirectoryChangesW` 监控配置文件目录
- 防抖：500ms 内多次变更只触发一次重载
- 验证失败时回滚到上一次有效配置
- 回调列表在锁内复制、在锁外调用，避免重入死锁
- 查询结果返回配置副本或持有快照的视图，禁止返回指向临时快照内部的裸指针

### 4.3 线程安全
- 配置存储使用 `std::atomic<std::shared_ptr<const Config>>`
- 读取：原子加载 shared_ptr，获取快照
- 写入：创建新 Config 对象，原子替换指针

---

## 五、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| Logger | 被依赖 | 配置加载/重载时记录日志 |
| 所有业务模块 | 依赖 | 读取各自配置 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
