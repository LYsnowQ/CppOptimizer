# 模块设计文档：GPU 心跳器 (GpuHeartbeat)

> **所属层**：Layer 3 - 应急响应层  
> **模块ID**：MOD-GPU-001  
> **状态**：Experimental（默认关闭）  
> **学习协作建议**：默认 L0/L1。AI 主导 D3D 资源生命周期、device removed、限流和真实负载门禁；学习者完成 Probe、fake、停止测试、指标采集和 A/B 分析。绿色区不产生 GPU workload，黄色不变量是可立即停止、资源逆序释放和硬负载上限；红色区是默认启用、后台持续渲染或宣称可以可靠锁定 P-State。

---

## 一、模块概述

### 1.1 职责
- 创建可严格限流的离屏 D3D 实验负载，用于验证特定设备是否存在收益
- 不承诺阻止 GPU 降频或锁定 P-State；驱动策略不可由 D3D workload 可靠控制
- 控制心跳强度（负载百分比），避免过度耗电
- 游戏退出后自动停止心跳

### 1.2 核心原理

GPU 电源管理（P-State）切换逻辑：
- 空闲时 → 低频率 P-State（节能）
- 负载时 → 高频率 P-State（性能）
- 滞回机制：进入高性能需要持续负载，退出只需要短暂空闲

任何持续渲染都会占用 GPU 时间、功耗和温度预算，并可能降低游戏可用 boost 空间。因此本模块只作为默认关闭的实验功能：先短时 Probe，再根据 GPU 利用率、供电和 A/B 数据决定是否继续；没有可重复收益时不得启用。

---

## 二、对外接口（大纲）

```cpp
namespace optimizer::gpu {

enum class GpuHeartbeatIntensity {
    Off,        // 停止心跳
    Minimal,    // ~1% GPU 负载
    Low,        // ~2-3% GPU 负载
    // 不提供 Medium 档；实验负载硬上限应控制在约 1% 内
};

class GpuHeartbeat {
public:
    bool Initialize();
    void Shutdown();
    bool SetIntensity(GpuHeartbeatIntensity intensity);
    double GetActualGpuLoad() const;
    bool SelectAdapter(uint32_t adapterIndex);

private:
    // D3D11 设备管理
    // 渲染线程
    // 帧率控制
};

} // namespace optimizer::gpu
```

---

## 三、内部架构（大纲）

```
┌─────────────────────────────────────────┐
│           GpuHeartbeat                  │
├─────────────────────────────────────────┤
│  ┌─────────────────────────────────┐   │
│  │      DXGI 工厂 & 适配器枚举        │   │
│  │  - CreateDXGIFactory2            │   │
│  │  - EnumAdaptersByGpuPreference   │   │
│  │  - 选择高性能 GPU                │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │      D3D11 设备创建               │   │
│  │  - D3D11CreateDevice             │   │
│  │  - 无窗口、无交换链               │   │
│  │  - 最小功能级别                  │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │      渲染心跳线程                 │   │
│  │  while (running) {              │   │
│  │    执行最小渲染工作负载          │   │
│  │    根据强度控制帧间隔            │   │
│  │    Sleep(间隔)                  │   │
│  │  }                              │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## 四、关键技术点

### 4.1 无窗口 D3D 设备创建
```cpp
D3D_FEATURE_LEVEL featureLevel;
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;

D3D11CreateDevice(
    adapter,                    // 指定 GPU
    D3D_DRIVER_TYPE_UNKNOWN,    // 使用适配器指定的驱动
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    nullptr, 0,                 // 默认功能级别
    D3D11_SDK_VERSION,
    &device,
    &featureLevel,
    &context
);
// 无 HWND，无交换链，纯计算/渲染到纹理
```

### 4.2 负载控制
```cpp
// 使用 waitable timer 限频而非忙循环；实际负载必须由 MetricsCollector 反馈闭环控制。
// 游戏 GPU 已繁忙、电池供电、远程会话、软件适配器或设备丢失时立即停止。
```

### 4.3 多 GPU 选择
```cpp
// 使用 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
// 或根据游戏进程所在的 GPU 选择同一设备
```

---

## 五、依赖模块

| 模块 | 关系 | 说明 |
|------|------|------|
| PolicyEngine | 依赖 | 接收强度调节指令 |
| MetricsCollector | 依赖 | 读取 GPU 负载反馈 |
| ProcessWatcher | 依赖 | 游戏退出触发停止 |

---

*文档版本：v0.1 | 创建日期：2026-08-08 | 状态：大纲阶段*
