#include "service/startup_entry.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <utility>

namespace optimizer::service {

namespace {

constexpr const wchar_t* kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// HKEY 的 RAII（只用于本文件的注册表句柄；释放函数固定为 RegCloseKey）。
class ScopedRegKey {
public:
    ScopedRegKey() = default;
    ~ScopedRegKey() noexcept {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
        }
    }
    ScopedRegKey(const ScopedRegKey&) = delete;
    ScopedRegKey& operator=(const ScopedRegKey&) = delete;

    [[nodiscard]] HKEY* AddressOf() noexcept { return &key_; }
    [[nodiscard]] HKEY Get() const noexcept { return key_; }
    [[nodiscard]] bool IsOpen() const noexcept { return key_ != nullptr; }

private:
    HKEY key_ = nullptr;
};

} // namespace

// Win32 后端：实现 StartupEntryBackend。当前阶段**委托给上面的自由函数**（避免重复实现），
// 后续切片再把注册表读写下沉到本类、由自由函数反向委托（分两步迁移，每步均可编译）。
class Win32StartupEntryBackend final : public StartupEntryBackend {
public:
    [[nodiscard]] optimizer::common::Result<std::wstring> Read() override {
        ScopedRegKey key;
        const LSTATUS opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                                               KEY_QUERY_VALUE, key.AddressOf());
        if (opened == ERROR_FILE_NOT_FOUND) {
            return optimizer::common::Result<std::wstring>::Success(std::wstring());
        }
        if (opened != ERROR_SUCCESS) {
            return optimizer::common::Result<std::wstring>::Failure(
                optimizer::common::Error::FromWin32(
                    static_cast<std::uint32_t>(opened),
                    "RegOpenKeyExW(startup run key)"));
        }
        DWORD type = 0;
        DWORD bytes = 0;
        LSTATUS queried = ::RegQueryValueExW(key.Get(), kStartupValueName, nullptr,
                                             &type, nullptr, &bytes);
        if (queried == ERROR_FILE_NOT_FOUND) {
            return optimizer::common::Result<std::wstring>::Success(std::wstring());
        }
        if (queried != ERROR_SUCCESS) {
            return optimizer::common::Result<std::wstring>::Failure(
                optimizer::common::Error::FromWin32(
                    static_cast<std::uint32_t>(queried), "RegQueryValueExW(size)"));
        }
        if (type != REG_SZ || bytes < sizeof(wchar_t)) {
            return optimizer::common::Result<std::wstring>::Failure(
                optimizer::common::Error::Validation("QueryStartupEntry",
                                                     L"自启动值类型不是 REG_SZ"));
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        queried = ::RegQueryValueExW(key.Get(), kStartupValueName, nullptr, &type,
                                     reinterpret_cast<BYTE*>(value.data()), &bytes);
        if (queried != ERROR_SUCCESS) {
            return optimizer::common::Result<std::wstring>::Failure(
                optimizer::common::Error::FromWin32(
                    static_cast<std::uint32_t>(queried), "RegQueryValueExW(value)"));
        }
        if (!value.empty() && value.back() == L'\0') {
            value.pop_back();
        }
        return optimizer::common::Result<std::wstring>::Success(std::move(value));
    }
    [[nodiscard]] optimizer::common::Result<void> Write(
        const std::wstring& quotedCommand) override {
        // 后端收到的就是**完整命令行**（已带引号），原样写入。
        ScopedRegKey key;
        const LSTATUS opened = ::RegCreateKeyExW(
            HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, key.AddressOf(), nullptr);
        if (opened != ERROR_SUCCESS) {
            return optimizer::common::Result<void>::Failure(
                optimizer::common::Error::FromWin32(
                    static_cast<std::uint32_t>(opened),
                    "RegCreateKeyExW(startup run key)"));
        }
        const DWORD bytes =
            static_cast<DWORD>((quotedCommand.size() + 1) * sizeof(wchar_t));
        const LSTATUS written = ::RegSetValueExW(
            key.Get(), kStartupValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(quotedCommand.c_str()), bytes);
        if (written != ERROR_SUCCESS) {
            return optimizer::common::Result<void>::Failure(
                optimizer::common::Error::FromWin32(
                    static_cast<std::uint32_t>(written),
                    "RegSetValueExW(startup entry)"));
        }
        return optimizer::common::Result<void>::Success();
    }
    [[nodiscard]] optimizer::common::Result<void> Remove() override {
        ScopedRegKey key;
        const LSTATUS opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                                               KEY_SET_VALUE, key.AddressOf());
        if (opened == ERROR_FILE_NOT_FOUND) {
            return optimizer::common::Result<void>::Success();
        }
        if (opened != ERROR_SUCCESS) {
            return optimizer::common::Result<void>::Failure(
                optimizer::common::Error::FromWin32(
                    static_cast<std::uint32_t>(opened),
                    "RegOpenKeyExW(startup run key)"));
        }
        const LSTATUS deleted = ::RegDeleteValueW(key.Get(), kStartupValueName);
        if (deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND) {
            return optimizer::common::Result<void>::Success(); // 不存在也视为成功（幂等）
        }
        return optimizer::common::Result<void>::Failure(
            optimizer::common::Error::FromWin32(
                static_cast<std::uint32_t>(deleted), "RegDeleteValueW(startup entry)"));
    }
};

optimizer::common::Result<void> InstallStartupEntry(StartupEntryBackend& backend,
                                                   const std::wstring& exePath) noexcept {
    if (exePath.empty()) {
        return optimizer::common::Result<void>::Failure(
            optimizer::common::Error::Validation("InstallStartupEntry",
                                                 L"可执行文件路径不能为空"));
    }
    // 带引号写入：路径含空格时仍可正确启动。
    return backend.Write(L"\"" + exePath + L"\"");
}

optimizer::common::Result<void> RemoveStartupEntry(
    StartupEntryBackend& backend) noexcept {
    return backend.Remove(); // 不存在视为成功（幂等）由后端保证
}

optimizer::common::Result<std::wstring> QueryStartupEntry(
    StartupEntryBackend& backend) noexcept {
    return backend.Read(); // 未注册 => 空串（Success）由后端保证
}

std::shared_ptr<StartupEntryBackend> CreateWin32StartupEntryBackend() noexcept {
    try {
        return std::make_shared<Win32StartupEntryBackend>();
    } catch (...) {
        return nullptr; // 分配失败：如实返回空
    }
}

common::Result<void> InstallStartupEntry(const std::wstring& exePath) noexcept {
    auto backend = CreateWin32StartupEntryBackend();
    if (!backend) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "InstallStartupEntry", L"无法创建注册表后端"));
    }
    return InstallStartupEntry(*backend, exePath);
}

common::Result<void> RemoveStartupEntry() noexcept {
    auto backend = CreateWin32StartupEntryBackend();
    if (!backend) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "RemoveStartupEntry", L"无法创建注册表后端"));
    }
    return RemoveStartupEntry(*backend);
}

common::Result<std::wstring> QueryStartupEntry() noexcept {
    auto backend = CreateWin32StartupEntryBackend();
    if (!backend) {
        return common::Result<std::wstring>::Failure(common::Error::Unsupported(
            "QueryStartupEntry", L"无法创建注册表后端"));
    }
    return QueryStartupEntry(*backend);
}

} // namespace optimizer::service
