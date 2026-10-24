#Requires -Version 5.1
<#
.SYNOPSIS
    注册 GitHub Actions 自托管 Windows runner（供 CI 的 build-windows 任务使用）。

.DESCRIPTION
    为什么需要自托管：本仓库不提交 thirdParty/（预编译 spdlog / tomlplusplus），
    GitHub 托管 runner 拉不到依赖，Windows 构建必然失败。

    使用步骤：
      1. 打开 https://github.com/LYsnowQ/CppOptimizer/settings/actions/runners/new?arch=x64&os=win
         复制页面上的 token（有效期约 1 小时，不要写进仓库）。
      2. 以管理员 PowerShell 运行：
           pwsh -File scripts/setup-runner.ps1 -Token <token>
      3. 回到页面确认 runner 状态为 Idle，标签包含 cppoptimizer。
      4. 需要卸载：pwsh -File scripts/setup-runner.ps1 -Uninstall

    注意：token 只在本进程内存中使用，不落盘；脚本不修改仓库文件。

.PARAMETER Token
    GitHub 页面给出的一次性注册 token。

.PARAMETER RunnerRoot
    runner 安装目录（默认 C:\actions-runner-cppoptimizer，放在仓库外）。

.PARAMETER Name
    runner 名称（默认 <主机名>-cppoptimizer）。

.PARAMETER Labels
    附加标签（逗号分隔），必须包含 cppoptimizer 以匹配 ci.yml。

.PARAMETER Version
    actions runner 版本；留空时自动查询最新 release。

.PARAMETER Uninstall
    停止并移除本机 runner（证书与配置留在 RunnerRoot，可手工删除）。
#>
param(
    [string] $Token = '',
    [string] $RunnerRoot = 'C:\actions-runner-cppoptimizer',
    [string] $Name = "$env:COMPUTERNAME-cppoptimizer",
    [string] $Labels = 'cppoptimizer,windows',
    [string] $Version = '',
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
$repoUrl = 'https://github.com/LYsnowQ/CppOptimizer'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    throw '需要管理员 PowerShell（安装 Windows 服务需要提权）。'
}

$configCmd = Join-Path $RunnerRoot 'config.cmd'
$svcCmd = Join-Path $RunnerRoot 'svc.cmd'

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $svcCmd)) {
        throw "在 $RunnerRoot 找不到 svc.cmd；请确认路径。"
    }
    Push-Location $RunnerRoot
    try {
        & .\svc.cmd stop
        & .\config.cmd remove --token $Token
    } finally {
        Pop-Location
    }
    Write-Output 'runner 已移除；RunnerRoot 可手工删除。'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw '缺少 -Token。请在 GitHub 仓库 Settings → Actions → Runners → New self-hosted runner 页面复制 token。'
}

New-Item -ItemType Directory -Path $RunnerRoot -Force | Out-Null
$zip = Join-Path $RunnerRoot 'actions-runner.zip'

if (-not (Test-Path -LiteralPath $configCmd)) {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/actions/runner/releases/latest' -UseBasicParsing
        $Version = $release.tag_name.TrimStart('v')
        Write-Output "最新 runner 版本: $Version"
    }
    $url = "https://github.com/actions/runner/releases/download/v$Version/actions-runner-win-x64-$Version.zip"
    Write-Output "下载 runner: $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -LiteralPath $zip -DestinationPath $RunnerRoot -Force
}

Push-Location $RunnerRoot
try {
    & .\config.cmd --url $repoUrl --token $Token --name $Name --labels $Labels --work _work --unattended
    if ($LASTEXITCODE -ne 0) { throw "config.cmd 失败（exit $LASTEXITCODE）" }
    & .\svc.cmd install
    if ($LASTEXITCODE -ne 0) { throw "svc.cmd install 失败（exit $LASTEXITCODE）" }
    & .\svc.cmd start
    if ($LASTEXITCODE -ne 0) { throw "svc.cmd start 失败（exit $LASTEXITCODE）" }
} finally {
    Pop-Location
}

Write-Output ''
Write-Output "完成：runner '$Name' 已注册并启动（标签 $Labels）。"
Write-Output "确认：$repoUrl/settings/actions/runners"
