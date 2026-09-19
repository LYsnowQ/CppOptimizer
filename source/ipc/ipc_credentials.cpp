#include "ipc/ipc_credentials.hpp"

#include "common/unique_resource.hpp"

#include <sddl.h>
#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace optimizer::ipc {

namespace {

// token 长度与字符集（与 ipc_facts schema / CLI IsValidIpcToken 一致）。
constexpr std::size_t kTokenLength = 32;

// SystemFunction036 (RtlGenRandom)：advapi32 导出，无需新增链接库；
// 比 std::random_device 更贴近加密随机要求。
extern "C" BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);

// 字母表：大写/小写/数字（62 个），按字节取模映射。
constexpr wchar_t kTokenAlphabet[] =
    L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

bool IsAsciiTokenChar(wchar_t ch) noexcept {
    const bool alnum =
        (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
        (ch >= L'0' && ch <= L'9');
    return alnum || ch == L'_' || ch == L'-';
}

bool IsValidTokenText(std::wstring_view text) noexcept {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    for (const wchar_t ch : text) {
        if (!IsAsciiTokenChar(ch)) {
            return false;
        }
    }
    return true;
}

std::wstring GenerateRandomToken() noexcept {
    std::array<BYTE, kTokenLength> bytes{};
    if (!SystemFunction036(bytes.data(),
                           static_cast<ULONG>(bytes.size()))) {
        return {};
    }
    std::wstring token;
    token.reserve(bytes.size());
    for (const BYTE b : bytes) {
        token.push_back(
            kTokenAlphabet[static_cast<std::size_t>(b) % 62]);
    }
    return token;
}

// 当前进程用户 SID 文本（OpenProcessToken+GetTokenInformation(TokenUser)）。
std::wstring CurrentUserSid() noexcept {
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return {};
    }
    common::UniqueHandle token(rawToken);
    DWORD needed = 0;
    (void)::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        return {};
    }
    std::vector<std::byte> buffer(needed);
    if (!::GetTokenInformation(token.Get(), TokenUser, buffer.data(), needed,
                               &needed)) {
        return {};
    }
    const auto& tokenUser =
        *reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (tokenUser.User.Sid == nullptr) {
        return {};
    }
    LPWSTR sidText = nullptr;
    if (!::ConvertSidToStringSidW(tokenUser.User.Sid, &sidText) ||
        sidText == nullptr) {
        return {};
    }
    std::unique_ptr<void, common::LocalFreeDeleter> sidGuard(sidText);
    return sidText;
}

// 收紧文件 DACL：保护 + 仅 SYSTEM 与当前用户（不放行 Everyone/Users 等）。
common::Result<void> RestrictFileDacl(const std::filesystem::path& path) noexcept {
    const std::wstring userSid = CurrentUserSid();
    if (userSid.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RestrictFileDacl", L"无法解析当前用户 SID"));
    }
    const std::wstring sddl =
        L"D:P(A;;FA;;;SY)(A;;FA;;;" + userSid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "ConvertStringSecurityDescriptorToSecurityDescriptorW"));
    }
    std::unique_ptr<void, common::LocalFreeDeleter> descriptorGuard(descriptor);
    if (!::SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                            descriptor)) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "SetFileSecurityW"));
    }
    return common::Result<void>::Success();
}

// 写文本文件（覆盖），返回失败信息。返回 false 表示失败（错误已含原因）。
bool WriteTokenFile(const std::filesystem::path& path,
                    std::wstring_view token) noexcept {
    common::UniqueHandle file(::CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.IsValid()) {
        return false;
    }
    std::string bytes;
    bytes.reserve(token.size());
    for (const wchar_t ch : token) {
        bytes.push_back(static_cast<char>(ch)); // token 为 ASCII
    }
    DWORD written = 0;
    return ::WriteFile(file.Get(), bytes.data(),
                       static_cast<DWORD>(bytes.size()), &written, nullptr) &&
           written == bytes.size();
}

common::Error ProvisionError(std::wstring message) {
    return common::Error::Validation("IpcCredentials", std::move(message));
}

} // namespace

std::filesystem::path DefaultAgentTokenFilePath() noexcept {
    wchar_t buffer[1024] = {};
    const DWORD len = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                static_cast<DWORD>(std::size(buffer)));
    if (len == 0 || len >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer)) / L"CppOptimizer" /
           L"ipc_agent_token";
}

common::Result<void> ProvisionAgentTokenFile(
    const std::filesystem::path& path, bool force) noexcept {
    if (path.empty()) {
        return common::Result<void>::Failure(
            ProvisionError(L"token 存储路径为空"));
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !force) {
        return common::Result<void>::Failure(
            ProvisionError(L"token 存储已存在（幂等；轮换请用 force）"));
    }
    // 创建父目录（已存在则忽略）。
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return common::Result<void>::Failure(
            ProvisionError(L"创建 token 存储父目录失败"));
    }
    const std::wstring token = GenerateRandomToken();
    if (token.empty()) {
        return common::Result<void>::Failure(
            ProvisionError(L"随机 token 生成失败"));
    }
    if (!WriteTokenFile(path, token)) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            ::GetLastError(), "WriteFile(token)"));
    }
    if (auto dacl = RestrictFileDacl(path); !dacl) {
        return common::Result<void>::Failure(dacl.ErrorValue());
    }
    return common::Result<void>::Success();
}

common::Result<std::wstring> ReadAgentTokenFile(
    const std::filesystem::path& path) noexcept {
    common::UniqueHandle file(::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.IsValid()) {
        return common::Result<std::wstring>::Failure(common::Error::FromWin32(
            ::GetLastError(), "CreateFileW(token)"));
    }
    std::string bytes;
    char buf[64];
    DWORD read = 0;
    while (::ReadFile(file.Get(), buf, sizeof(buf), &read, nullptr) &&
           read > 0) {
        bytes.append(buf, read);
    }
    // 去尾部空白（CR/LF/空格）。
    while (!bytes.empty() &&
           (bytes.back() == '\r' || bytes.back() == '\n' ||
            bytes.back() == ' ' || bytes.back() == '\t')) {
        bytes.pop_back();
    }
    if (bytes.empty() || bytes.size() > 64) {
        return common::Result<std::wstring>::Failure(
            ProvisionError(L"token 存储内容长度非法"));
    }
    std::wstring token;
    token.reserve(bytes.size());
    for (const char ch : bytes) {
        const wchar_t wch = static_cast<unsigned char>(ch);
        if (!IsAsciiTokenChar(wch)) {
            return common::Result<std::wstring>::Failure(
                ProvisionError(L"token 存储内容含非法字符"));
        }
        token.push_back(wch);
    }
    return common::Result<std::wstring>::Success(std::move(token));
}

bool IsAgentTokenProvisioned(const std::filesystem::path& path) noexcept {
    return static_cast<bool>(ReadAgentTokenFile(path));
}

} // namespace optimizer::ipc
