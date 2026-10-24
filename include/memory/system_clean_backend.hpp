#pragma once

#include "memory/memory_cleaner.hpp"

#include <cstdint>
#include <optional>

namespace optimizer::memory {

// R3 步骤的真实后端（**仅限隔离环境**；调用方必须先通过六道门与隔离确认）。
// 已实现：Standby/Modified List 清理（`NtSetSystemInformation` +
// `SystemMemoryListInformation` / `MemoryPurgeStandbyList`），需要
// `SeProfileSingleProcessPrivilege`（管理员）。
// 未实现：系统文件缓存修剪（S4）——继续如实返回 Unsupported，绝不伪装成功。
class SystemMemoryCleanBackend final : public CleanBackend {
public:
    [[nodiscard]] common::Result<void> Execute(CleanKind kind) override;
};

// 只读探测：当前令牌是否**包含**该步骤所需特权（不启用、不改令牌、不执行系统调用）。
// WorkingSetTrim 不需要特权（恒 true）；S4 尚未实现（恒 false）。
[[nodiscard]] bool HasRequiredPrivilege(CleanKind kind) noexcept;

// 只读：当前 Standby（备用）列表占用的字节数（`NtQuerySystemInformation` +
// `SystemMemoryListInformation`，按页数 × 页大小换算）。
// 这是 S3 清理**效果**的权威读数：清理成功的判据是该值在执行后显著下降。
// 查询失败或返回结构尺寸不符时返回 nullopt——**不把“没读到”写成 0**。
[[nodiscard]] std::optional<std::uint64_t> QueryStandbyBytes() noexcept;

} // namespace optimizer::memory
