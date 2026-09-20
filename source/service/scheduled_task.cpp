#include "service/scheduled_task.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <taskschd.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <utility>

namespace optimizer::service {

namespace {

using Microsoft::WRL::ComPtr;

// COM 初始化 RAII：后端可能被任意线程调用，必须由调用线程完成初始化。
// S_FALSE（本线程此前已初始化）与 RPC_E_CHANGED_MODE（已按其它套间模型初始化）都视为可用，
// 但只有本对象真正执行了初始化时才在析构里 CoUninitialize——不破坏调用线程的 COM 状态。
class ScopedCom {
public:
    ScopedCom() noexcept {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            ready_ = true;
            owns_ = hr != S_FALSE;
        } else if (hr == RPC_E_CHANGED_MODE) {
            ready_ = true;
            owns_ = false;
        }
    }
    ~ScopedCom() noexcept {
        if (owns_) {
            ::CoUninitialize();
        }
    }
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    [[nodiscard]] bool IsReady() const noexcept { return ready_; }

private:
    bool ready_ = false;
    bool owns_ = false;
};

// BSTR 的 RAII（SysAllocString/SysFreeString 必须配对；失败不泄漏）。
class ScopedBstr {
public:
    explicit ScopedBstr(const wchar_t* text) noexcept
        : value_(text != nullptr ? ::SysAllocString(text) : nullptr) {}
    ~ScopedBstr() noexcept {
        if (value_ != nullptr) {
            ::SysFreeString(value_);
        }
    }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    [[nodiscard]] BSTR Get() const noexcept { return value_; }
    [[nodiscard]] bool IsValid() const noexcept { return value_ != nullptr; }

private:
    BSTR value_ = nullptr;
};

common::Error FromHresult(HRESULT hr, const char* operation) {
    return common::Error::FromHResult(static_cast<std::int32_t>(hr), operation);
}

// 内部错误（域为 Internal）：仅用于“本进程内部状态不成立”，不用于系统调用失败。
common::Error InternalError(std::string operation, std::wstring message) {
    common::Error error;
    error.domain = common::ErrorDomain::Internal;
    error.operation = std::move(operation);
    error.message = std::move(message);
    return error;
}

// VARIANT 的 RAII：任务触发器/空闲设置等可选参数需要 VARIANT 载体。
class ScopedVariant {
public:
    ScopedVariant() noexcept { ::VariantInit(&value_); }
    ~ScopedVariant() noexcept { ::VariantClear(&value_); }
    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;

    [[nodiscard]] VARIANT* AddressOf() noexcept { return &value_; }
    [[nodiscard]] VARIANT Value() const noexcept { return value_; }

private:
    VARIANT value_{};
};

// 连接任务计划服务的根文件夹。非提升进程通常可读；写入（注册）需要管理员。
common::Result<ComPtr<ITaskFolder>> ConnectRootFolder() {
    ComPtr<ITaskService> service;
    HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(service.GetAddressOf()));
    if (FAILED(hr)) {
        return common::Result<ComPtr<ITaskFolder>>::Failure(
            FromHresult(hr, "CoCreateInstance(TaskScheduler)"));
    }
    ScopedVariant emptyUser;
    ScopedVariant emptyDomain;
    ScopedVariant emptyPassword;
    ScopedVariant emptySddl;
    hr = service->Connect(emptyUser.Value(), emptyDomain.Value(),
                          emptyPassword.Value(), emptySddl.Value());
    if (FAILED(hr)) {
        return common::Result<ComPtr<ITaskFolder>>::Failure(
            FromHresult(hr, "ITaskService::Connect"));
    }
    ScopedBstr rootPath(L"\\");
    if (!rootPath.IsValid()) {
        return common::Result<ComPtr<ITaskFolder>>::Failure(InternalError(
            "ConnectRootFolder", L"分配任务计划服务路径失败"));
    }
    ComPtr<ITaskFolder> root;
    hr = service->GetFolder(rootPath.Get(), root.GetAddressOf());
    if (FAILED(hr)) {
        return common::Result<ComPtr<ITaskFolder>>::Failure(
            FromHresult(hr, "ITaskService::GetFolder"));
    }
    return common::Result<ComPtr<ITaskFolder>>::Success(std::move(root));
}

std::wstring TriggerTextFromType(TASK_TRIGGER_TYPE2 type) {
    switch (type) {
        case TASK_TRIGGER_LOGON:
            return L"logon";
        case TASK_TRIGGER_BOOT:
            return L"boot";
        default:
            return std::wstring();
    }
}

// COM 后端：ITaskService（Task Scheduler 2.0）。只创建**当前用户交互令牌**任务，
// 不请求提升、不设置最高运行级别。
class Win32ScheduledTaskBackend final : public ScheduledTaskBackend {
public:
    [[nodiscard]] common::Result<ScheduledTaskStatus> Read(
        const std::wstring& taskName) override {
        ScopedCom com;
        if (!com.IsReady()) {
            return common::Result<ScheduledTaskStatus>::Failure(
                FromHresult(CO_E_NOTINITIALIZED, "CoInitializeEx(task scheduler)"));
        }
        auto root = ConnectRootFolder();
        if (!root) {
            return common::Result<ScheduledTaskStatus>::Failure(
                root.ErrorValue());
        }
        return ReadFromFolder(root.Value().Get(), taskName);
    }
    [[nodiscard]] common::Result<void> Create(
        const ScheduledTaskSpec& spec) override {
        ScopedCom com;
        if (!com.IsReady()) {
            return common::Result<void>::Failure(
                FromHresult(CO_E_NOTINITIALIZED, "CoInitializeEx(task scheduler)"));
        }
        ComPtr<ITaskService> service;
        HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(service.GetAddressOf()));
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "CoCreateInstance(TaskScheduler)"));
        }
        ScopedVariant emptyUser;
        ScopedVariant emptyDomain;
        ScopedVariant emptyPassword;
        ScopedVariant emptySddl;
        hr = service->Connect(emptyUser.Value(), emptyDomain.Value(),
                              emptyPassword.Value(), emptySddl.Value());
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskService::Connect"));
        }
        ComPtr<ITaskDefinition> definition;
        hr = service->NewTask(0, definition.GetAddressOf());
        if (FAILED(hr) || !definition) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskService::NewTask"));
        }
        if (auto prepared = PrepareDefinition(definition.Get(), spec); !prepared) {
            return prepared;
        }
        ScopedBstr rootPath(L"\\");
        ScopedBstr taskPath(spec.taskName.c_str());
        if (!rootPath.IsValid() || !taskPath.IsValid()) {
            return common::Result<void>::Failure(InternalError(
                "Create", L"分配任务路径失败"));
        }
        ComPtr<ITaskFolder> root;
        hr = service->GetFolder(rootPath.Get(), root.GetAddressOf());
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskService::GetFolder"));
        }
        ComPtr<IRegisteredTask> registered;
        ScopedVariant userId;   // 空 = 当前用户
        ScopedVariant password; // 交互令牌模式不需要密码
        ScopedVariant sddl;     // 空 = 默认安全描述符
        hr = root->RegisterTaskDefinition(
            taskPath.Get(), definition.Get(), TASK_CREATE_OR_UPDATE,
            userId.Value(), password.Value(), TASK_LOGON_INTERACTIVE_TOKEN,
            sddl.Value(), registered.GetAddressOf());
        if (FAILED(hr)) {
            // 非提升进程通常在此处得到拒绝访问：如实返回，不伪装成功。
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskFolder::RegisterTaskDefinition"));
        }
        return common::Result<void>::Success();
    }

    [[nodiscard]] common::Result<void> Remove(
        const std::wstring& taskName) override {
        ScopedCom com;
        if (!com.IsReady()) {
            return common::Result<void>::Failure(
                FromHresult(CO_E_NOTINITIALIZED, "CoInitializeEx(task scheduler)"));
        }
        auto root = ConnectRootFolder();
        if (!root) {
            return common::Result<void>::Failure(root.ErrorValue());
        }
        ScopedBstr taskPath(taskName.c_str());
        if (!taskPath.IsValid()) {
            return common::Result<void>::Failure(InternalError(
                "Remove", L"分配任务路径失败"));
        }
        const HRESULT hr = root.Value()->DeleteTask(taskPath.Get(), 0);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return common::Result<void>::Success(); // 不存在也视为成功（幂等）
        }
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskFolder::DeleteTask"));
        }
        return common::Result<void>::Success();
    }

private:
    static common::Result<ScheduledTaskStatus> ReadFromFolder(
        ITaskFolder* root, const std::wstring& taskName) {
        ScopedBstr taskPath(taskName.c_str());
        if (!taskPath.IsValid()) {
            return common::Result<ScheduledTaskStatus>::Failure(
                InternalError("Read", L"分配任务路径失败"));
        }
        ComPtr<IRegisteredTask> task;
        HRESULT hr = root->GetTask(taskPath.Get(), task.GetAddressOf());
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            // 未注册不是错误：installed=false 如实表达“无此任务”。
            return common::Result<ScheduledTaskStatus>::Success(
                ScheduledTaskStatus{});
        }
        if (FAILED(hr)) {
            return common::Result<ScheduledTaskStatus>::Failure(
                FromHresult(hr, "ITaskFolder::GetTask"));
        }
        ScheduledTaskStatus status;
        status.installed = true;
        ComPtr<ITaskDefinition> definition;
        if (SUCCEEDED(task->get_Definition(definition.GetAddressOf())) &&
            definition) {
            status.commandLine = ReadCommandLine(definition.Get());
            status.trigger = ReadTrigger(definition.Get());
        }
        // 定义读取失败时留空：如实表示“任务存在但概要未知”，不伪造命令行。
        return common::Result<ScheduledTaskStatus>::Success(std::move(status));
    }

    // 读出可执行路径与参数（拼接为命令行）；任一步失败返回空串（如实表示未知，不伪造）。
    static std::wstring ReadCommandLine(ITaskDefinition* definition) {
        ComPtr<IActionCollection> actions;
        if (FAILED(definition->get_Actions(actions.GetAddressOf())) || !actions) {
            return std::wstring();
        }
        LONG count = 0;
        if (FAILED(actions->get_Count(&count)) || count <= 0) {
            return std::wstring();
        }
        ComPtr<IAction> action;
        if (FAILED(actions->get_Item(1, action.GetAddressOf())) || !action) {
            return std::wstring();
        }
        ComPtr<IExecAction> exec;
        if (FAILED(action.As(&exec)) || !exec) {
            return std::wstring();
        }
        std::wstring commandLine;
        BSTR path = nullptr;
        if (SUCCEEDED(exec->get_Path(&path)) && path != nullptr) {
            commandLine.assign(path);
            ::SysFreeString(path);
        }
        BSTR arguments = nullptr;
        if (SUCCEEDED(exec->get_Arguments(&arguments)) && arguments != nullptr) {
            if (!commandLine.empty()) {
                commandLine += L' ';
            }
            commandLine.append(arguments);
            ::SysFreeString(arguments);
        }
        return commandLine;
    }

    static std::wstring ReadTrigger(ITaskDefinition* definition) {
        ComPtr<ITriggerCollection> triggers;
        if (FAILED(definition->get_Triggers(triggers.GetAddressOf())) ||
            !triggers) {
            return std::wstring();
        }
        LONG count = 0;
        if (FAILED(triggers->get_Count(&count)) || count <= 0) {
            return std::wstring();
        }
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->get_Item(1, trigger.GetAddressOf())) || !trigger) {
            return std::wstring();
        }
        TASK_TRIGGER_TYPE2 type = TASK_TRIGGER_EVENT;
        if (FAILED(trigger->get_Type(&type))) {
            return std::wstring();
        }
        return TriggerTextFromType(type);
    }

    // 组装任务定义：登录触发 + 当前用户交互令牌 + 错过触发后补跑，不设置最高运行级别。
    static common::Result<void> PrepareDefinition(ITaskDefinition* definition,
                                                  const ScheduledTaskSpec& spec) {
        HRESULT hr = S_OK;
        ComPtr<IRegistrationInfo> registration;
        if (SUCCEEDED(definition->get_RegistrationInfo(
                registration.GetAddressOf())) &&
            registration) {
            ScopedBstr author(L"CppOptimizer");
            if (!author.IsValid()) {
                return common::Result<void>::Failure(InternalError(
                    "PrepareDefinition", L"分配作者字段失败"));
            }
            hr = registration->put_Author(author.Get());
            if (FAILED(hr)) {
                return common::Result<void>::Failure(
                    FromHresult(hr, "IRegistrationInfo::put_Author"));
            }
        }
        ComPtr<ITaskSettings> settings;
        hr = definition->get_Settings(settings.GetAddressOf());
        if (FAILED(hr) || !settings) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskDefinition::get_Settings"));
        }
        ScopedBstr noLimit(L"PT0S"); // 常驻进程：不设执行时限
        if (!noLimit.IsValid()) {
            return common::Result<void>::Failure(
                InternalError("PrepareDefinition", L"分配时限字段失败"));
        }
        // 逐项检查：设置未生效时不得继续注册出一个“看起来成功但语义不符”的任务。
        hr = settings->put_StartWhenAvailable(VARIANT_TRUE);
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskSettings::put_StartWhenAvailable"));
        }
        hr = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskSettings::put_DisallowStartIfOnBatteries"));
        }
        hr = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskSettings::put_StopIfGoingOnBatteries"));
        }
        hr = settings->put_ExecutionTimeLimit(noLimit.Get());
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskSettings::put_ExecutionTimeLimit"));
        }
        ComPtr<ITriggerCollection> triggers;
        hr = definition->get_Triggers(triggers.GetAddressOf());
        if (FAILED(hr) || !triggers) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskDefinition::get_Triggers"));
        }
        ComPtr<ITrigger> trigger;
        hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.GetAddressOf());
        if (FAILED(hr) || !trigger) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITriggerCollection::Create(logon)"));
        }
        ComPtr<IActionCollection> actions;
        hr = definition->get_Actions(actions.GetAddressOf());
        if (FAILED(hr) || !actions) {
            return common::Result<void>::Failure(
                FromHresult(hr, "ITaskDefinition::get_Actions"));
        }
        ComPtr<IAction> action;
        hr = actions->Create(TASK_ACTION_EXEC, action.GetAddressOf());
        if (FAILED(hr) || !action) {
            return common::Result<void>::Failure(
                FromHresult(hr, "IActionCollection::Create(exec)"));
        }
        ComPtr<IExecAction> exec;
        hr = action.As(&exec);
        if (FAILED(hr) || !exec) {
            return common::Result<void>::Failure(
                FromHresult(hr, "IAction::QueryInterface(IExecAction)"));
        }
        const std::wstring& commandLine = spec.commandLine;
        // 命令行格式约定："<exe>" <args...>：首个带引号的片段为可执行文件，其余为参数。
        std::wstring executable;
        std::wstring arguments;
        if (!commandLine.empty() && commandLine.front() == L'"') {
            const auto closing = commandLine.find(L'"', 1);
            if (closing != std::wstring::npos) {
                executable = commandLine.substr(1, closing - 1);
                arguments = commandLine.substr(closing + 1);
                while (!arguments.empty() && arguments.front() == L' ') {
                    arguments.erase(arguments.begin());
                }
            }
        }
        if (executable.empty()) {
            // 无引号（或引号不成对）：退回“整串作为可执行路径”，不猜测拆分。
            executable = commandLine;
            arguments.clear();
        }
        ScopedBstr execPath(executable.c_str());
        ScopedBstr execArgs(arguments.c_str());
        if (!execPath.IsValid() || !execArgs.IsValid()) {
            return common::Result<void>::Failure(InternalError(
                "PrepareDefinition", L"分配执行动作参数失败"));
        }
        hr = exec->put_Path(execPath.Get());
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "IExecAction::put_Path"));
        }
        hr = exec->put_Arguments(execArgs.Get());
        if (FAILED(hr)) {
            return common::Result<void>::Failure(
                FromHresult(hr, "IExecAction::put_Arguments"));
        }
        return common::Result<void>::Success();
    }
};

} // namespace

common::Result<void> InstallScheduledTask(ScheduledTaskBackend& backend,
                                          const ScheduledTaskSpec& spec) noexcept {
    if (spec.taskName.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "InstallScheduledTask", L"任务名不能为空"));
    }
    if (spec.taskName.find(L'\\') != std::wstring::npos) {
        // 任务名是根文件夹下的名字，不是路径：含分隔符会隐式指向子文件夹。
        return common::Result<void>::Failure(common::Error::Validation(
            "InstallScheduledTask", L"任务名不能包含路径分隔符"));
    }
    if (spec.commandLine.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "InstallScheduledTask", L"命令行不能为空"));
    }
    if (spec.trigger != TaskTrigger::OnLogon) {
        // 开机触发需以 SYSTEM 运行，与本项目的权限分界冲突：显式拒绝，不静默提权。
        return common::Result<void>::Failure(common::Error::Validation(
            "InstallScheduledTask",
            L"当前仅支持登录触发；开机触发需以 SYSTEM 运行，与“托盘与 Agent 进程永不提权”的分界冲突"));
    }
    return backend.Create(spec);
}

common::Result<void> RemoveScheduledTask(ScheduledTaskBackend& backend,
                                         const std::wstring& taskName) noexcept {
    if (taskName.empty()) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RemoveScheduledTask", L"任务名不能为空"));
    }
    if (taskName.find(L'\\') != std::wstring::npos) {
        return common::Result<void>::Failure(common::Error::Validation(
            "RemoveScheduledTask", L"任务名不能包含路径分隔符"));
    }
    return backend.Remove(taskName); // 不存在视为成功（幂等）由后端保证
}

common::Result<ScheduledTaskStatus> QueryScheduledTask(
    ScheduledTaskBackend& backend, const std::wstring& taskName) noexcept {
    if (taskName.empty()) {
        return common::Result<ScheduledTaskStatus>::Failure(
            common::Error::Validation("QueryScheduledTask", L"任务名不能为空"));
    }
    if (taskName.find(L'\\') != std::wstring::npos) {
        return common::Result<ScheduledTaskStatus>::Failure(
            common::Error::Validation("QueryScheduledTask",
                                      L"任务名不能包含路径分隔符"));
    }
    return backend.Read(taskName); // 未注册 => installed=false（Success）由后端保证
}

std::shared_ptr<ScheduledTaskBackend> CreateWin32ScheduledTaskBackend() noexcept {
    try {
        return std::make_shared<Win32ScheduledTaskBackend>();
    } catch (...) {
        return nullptr; // 分配失败：如实返回空
    }
}

common::Result<void> InstallScheduledTask(const ScheduledTaskSpec& spec) noexcept {
    auto backend = CreateWin32ScheduledTaskBackend();
    if (!backend) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "InstallScheduledTask", L"无法创建任务计划后端"));
    }
    return InstallScheduledTask(*backend, spec);
}

common::Result<void> RemoveScheduledTask(const std::wstring& taskName) noexcept {
    auto backend = CreateWin32ScheduledTaskBackend();
    if (!backend) {
        return common::Result<void>::Failure(common::Error::Unsupported(
            "RemoveScheduledTask", L"无法创建任务计划后端"));
    }
    return RemoveScheduledTask(*backend, taskName);
}

common::Result<ScheduledTaskStatus> QueryScheduledTask(
    const std::wstring& taskName) noexcept {
    auto backend = CreateWin32ScheduledTaskBackend();
    if (!backend) {
        return common::Result<ScheduledTaskStatus>::Failure(
            common::Error::Unsupported("QueryScheduledTask",
                                       L"无法创建任务计划后端"));
    }
    return QueryScheduledTask(*backend, taskName);
}

} // namespace optimizer::service
