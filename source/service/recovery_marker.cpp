#include "service/recovery_marker.hpp"

#include <windows.h>

#include <fstream>
#include <string>

namespace optimizer::service {

std::filesystem::path DefaultRecoveryMarkerPath() noexcept {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                static_cast<DWORD>(MAX_PATH));
    std::filesystem::path root;
    if (len > 0 && len < static_cast<DWORD>(MAX_PATH)) {
        root = std::filesystem::path(buffer);
    } else {
        // 回退：系统 Temp（极少出现，避免空路径）。
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec);
    }
    return root / L"CppOptimizer" / L"recovery-state.json";
}

common::Result<void> WriteRecoveryMarker(
    const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                static_cast<std::uint32_t>(ec.value()),
                "create_directories(recovery marker)"));
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open recovery marker for write"));
    }
    std::string content(kRecoveryMarkerEnvelope);
    content += "\nunclean\n";
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (!out) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "flush recovery marker"));
    }
    return common::Result<void>::Success();
}

common::Result<void> ClearRecoveryMarker(
    const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
        return common::Result<void>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(ec.value()),
            "remove recovery marker"));
    }
    (void)removed; // 不存在视为成功（幂等清除）
    return common::Result<void>::Success();
}

common::Result<bool> IsRecoveryMarkerSet(
    const std::filesystem::path& path) noexcept {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // 打不开：若因不存在则属“未设置”；其它 IO 错误如实上报。
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec) {
            return common::Result<bool>::Success(false);
        }
        return common::Result<bool>::Failure(common::Error::FromWin32(
            static_cast<std::uint32_t>(::GetLastError()),
            "open recovery marker for read"));
    }
    std::string firstLine;
    std::getline(in, firstLine);
    if (!firstLine.empty() && firstLine.back() == '\r') {
        firstLine.pop_back(); // 容忍 CRLF
    }
    return common::Result<bool>::Success(firstLine == kRecoveryMarkerEnvelope);
}

RecoveryAnomalyAction DecideRecoveryAnomalyAction(bool markerSet,
                                                  bool confirmRecovery,
                                                  bool latchEnabled) noexcept {
    if (!markerSet || confirmRecovery) {
        return RecoveryAnomalyAction::None; // 无待处理异常（或已被显式确认）
    }
    // 配置可关闭“阻断”，但不能关闭“上报”：关闭时返回 NoteOnly，调用方仍需留痕。
    return latchEnabled ? RecoveryAnomalyAction::Latch
                        : RecoveryAnomalyAction::NoteOnly;
}

} // namespace optimizer::service
