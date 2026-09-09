#include "service/tray_host.hpp"

#include <shellapi.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace optimizer::service {

namespace {

// 通知区回调消息号（NIF_MESSAGE）：任务栏把鼠标/菜单事件以该消息发给隐藏窗口。
inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;
// 托盘图标标识（本宿主每进程单图标）。
inline constexpr UINT kTrayIconId = 1;

// 真实 Win32 托盘图标后端：Shell_NotifyIconW 添加/移除通知区图标。
class Win32TrayIconBackend final : public TrayIconBackend {
public:
    common::Result<void> AddIcon(void* hwnd,
                                 const std::wstring& tooltip) override {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = static_cast<HWND>(hwnd);
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayCallbackMessage;
        nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
        // 悬停提示上限 127 宽字符（NIF_TIP 缓冲 128 含终止符）。
        if (!tooltip.empty()) {
            const std::size_t copied =
                tooltip.copy(nid.szTip, std::size(nid.szTip) - 1);
            nid.szTip[copied] = L'\0';
        }
        if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
            return common::Result<void>::Failure(common::Error::FromWin32(
                ::GetLastError(), "Shell_NotifyIconW(NIM_ADD)"));
        }
        return common::Result<void>::Success();
    }

    void RemoveIcon(void* hwnd) noexcept override {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = static_cast<HWND>(hwnd);
        nid.uID = kTrayIconId;
        // 移除尽力而为：图标缺失/任务栏异常时 NIM_DELETE 失败可忽略
        //（窗口/进程销毁时系统会清理通知区图标）。
        (void)::Shell_NotifyIconW(NIM_DELETE, &nid);
    }
};

} // namespace

std::shared_ptr<TrayIconBackend> CreateWin32TrayIconBackend() {
    return std::make_shared<Win32TrayIconBackend>();
}

TrayHost::TrayHost(Options options)
    : options_(std::move(options)) {}

TrayHost::~TrayHost() noexcept {
    Stop();
}

common::Result<void> TrayHost::Start(
    ExitCallback onExit,
    std::shared_ptr<TrayIconBackend> backend) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Idle) {
            return common::Result<void>::Failure(common::Error::Validation(
                "TrayHost::Start", L"托盘宿主已在运行或启动中"));
        }
        state_ = State::Starting;
        hwnd_ = nullptr;
        iconAdded_ = false;
        startError_.reset();
    }
    if (!backend) {
        backend = CreateWin32TrayIconBackend();
    }
    stopRequested_.store(false);
    exitRequested_ = false;
    try {
        worker_ = std::thread([this, onExit = std::move(onExit),
                               backend = std::move(backend)]() mutable {
            Worker(onExit, backend);
        });
    } catch (const std::system_error&) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Idle;
        return common::Result<void>::Failure(common::Error::Validation(
            "TrayHost::Start", L"无法创建托盘 UI 线程"));
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待图标就绪/失败（窗口创建与 Shell_NotifyIcon 均为毫秒级）。
        cv_.wait_for(lock, std::chrono::seconds(5),
                     [this] { return state_ != State::Starting; });
    }
    if (state_ == State::Running) {
        return common::Result<void>::Success();
    }
    // 启动失败：回收线程并把错误回传（失败不伪装成功）。
    const common::Error error =
        startError_ ? *startError_
                    : common::Error::Validation("TrayHost::Start",
                                                L"托盘启动超时");
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
    return common::Result<void>::Failure(error);
}

void TrayHost::Stop() noexcept {
    RequestStopFromAnyThread();
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.joinable() &&
            worker_.get_id() != std::this_thread::get_id()) {
            // 移出到局部再 join，避免停止路径与析构相互竞态。
            toJoin = std::move(worker_);
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

TrayHost::State TrayHost::GetState() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool TrayHost::IsIconAdded() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return iconAdded_;
}

void* TrayHost::WindowHandle() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return hwnd_;
}

void TrayHost::RequestStopFromAnyThread() noexcept {
    stopRequested_.store(true);
    void* hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd = hwnd_;
    }
    if (hwnd != nullptr) {
        // 通知 UI 线程退出（投递失败可忽略：窗口可能刚销毁）。
        (void)::PostMessageW(static_cast<HWND>(hwnd), WM_CLOSE, 0, 0);
    }
    // 窗口尚未创建时，由 Worker 发布成功后自检 stopRequested_ 再补发关闭。
}

void TrayHost::CloseFromUiThread() noexcept {
    void* hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd = hwnd_;
    }
    if (hwnd == nullptr) {
        return;
    }
    if (backend_ && iconAdded_) {
        backend_->RemoveIcon(hwnd);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        iconAdded_ = false;
    }
    ::DestroyWindow(static_cast<HWND>(hwnd));
}

void TrayHost::RequestExitFromUiThread() noexcept {
    const bool first = !exitRequested_.exchange(true);
    if (first && onExit_) {
        onExit_(); // 菜单“退出”：通知业务宿主停止（回调必须快速返回）
    }
    // 无论是否设置了回调，都关闭窗口结束托盘线程（业务侧随后调用 Stop 幂等收尾）。
    if (hwnd_ != nullptr) {
        (void)::PostMessageW(static_cast<HWND>(hwnd_), WM_CLOSE, 0, 0);
    }
}

void TrayHost::ShowTrayMenu(HWND hwnd) noexcept {
    POINT cursor{};
    if (!::GetCursorPos(&cursor)) {
        return;
    }
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING, kTrayExitCommandId, L"退出(&X)");
    ::SetForegroundWindow(hwnd);
    const UINT command =
        ::TrackPopupMenu(menu,
                         TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN |
                             TPM_TOPALIGN,
                         cursor.x, cursor.y, 0, hwnd, nullptr);
    // 再发一条空消息，保证菜单关闭后本窗口仍可重新弹出菜单。
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    if (command == kTrayExitCommandId) {
        RequestExitFromUiThread();
    }
}

LRESULT CALLBACK TrayHost::WindowProc(HWND hwnd, UINT message,
                                      WPARAM wParam,
                                      LPARAM lParam) noexcept {
    // 实例指针在 CreateWindow 后经 GWLP_USERDATA 存取；早期系统消息（如 WM_NCCREATE）
    // 到达时指针未设置，按默认处理即可。
    auto* self = reinterpret_cast<TrayHost*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case kTrayCallbackMessage:
            if (self != nullptr && LOWORD(lParam) == WM_RBUTTONUP) {
                self->ShowTrayMenu(hwnd); // 通知区图标右键
            }
            break;
        case WM_COMMAND:
            if (self != nullptr &&
                LOWORD(wParam) == kTrayExitCommandId) {
                self->RequestExitFromUiThread();
            }
            break;
        case WM_CLOSE:
            if (self != nullptr) {
                self->CloseFromUiThread(); // 移除图标 -> 销毁窗口
            } else {
                ::DestroyWindow(hwnd);
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            break;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void TrayHost::Worker(ExitCallback onExit,
                      std::shared_ptr<TrayIconBackend> backend) noexcept {
    onExit_ = std::move(onExit);
    backend_ = std::move(backend);

    std::optional<common::Error> startError;
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    // 注册隐藏窗口类（ALREADY_EXISTS = 本进程先前实例注册过，沿用即可）。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayHost::WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = options_.windowClass.c_str();
    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        startError = common::Error::FromWin32(
            ::GetLastError(), "RegisterClassExW(tray window)");
    }

    HWND hwnd = nullptr;
    if (!startError) {
        // 隐藏顶层窗口（不 ShowWindow，任务栏不出现窗口条目；仅接收托盘消息）。
        hwnd = ::CreateWindowExW(
            0, options_.windowClass.c_str(), options_.windowTitle.c_str(),
            WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
        if (hwnd == nullptr) {
            startError = common::Error::FromWin32(
                ::GetLastError(), "CreateWindowExW(tray window)");
        } else {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(this));
        }
    }

    bool iconAdded = false;
    if (hwnd != nullptr && backend_) {
        if (auto added = backend_->AddIcon(hwnd, options_.tooltip); !added) {
            startError = added.ErrorValue();
        } else {
            iconAdded = true;
        }
    }
    if (startError && hwnd != nullptr) {
        ::DestroyWindow(hwnd);
        hwnd = nullptr;
        iconAdded = false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd_ = hwnd;
        iconAdded_ = iconAdded;
        state_ = startError ? State::Idle : State::Running;
        startError_ = startError;
    }
    cv_.notify_all();
    if (startError) {
        // 启动失败：注销窗口类并退出（Start 已 join 回收本线程）。
        ::UnregisterClassW(options_.windowClass.c_str(), instance);
        onExit_ = nullptr;
        backend_.reset();
        return;
    }

    // 停止请求可能早于窗口就绪：补发关闭消息让消息循环走统一退出路径。
    if (stopRequested_.load()) {
        (void)::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // 消息循环结束：窗口已在 WM_CLOSE/WM_DESTROY 路径销毁并移除图标。
    ::UnregisterClassW(options_.windowClass.c_str(), instance);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd_ = nullptr;
        iconAdded_ = false;
        state_ = State::Idle;
    }
    cv_.notify_all();
    onExit_ = nullptr;
    backend_.reset();
}

} // namespace optimizer::service
