#Requires -Version 5.1
<#
.SYNOPSIS
    CppOptimizer 提交前门禁（本地钩子、CI 与人工共用一套实现）。

.DESCRIPTION
    -Fast  : 提交信息 + 仓库卫生（编码、禁止路径、diff --check、公开面）。
    默认    : -Fast 全部 + 三构建 + 全量 ctest。

    退出码 0 = 全部通过；1 = 至少一项失败（逐项打印 FAIL 与原因）。

.PARAMETER Fast
    只跑静态检查，不构建。

.PARAMETER Ci
    CI 模式：提交范围取 GITHUB_BASE_REF（PR）或 HEAD~1（push）。

.PARAMETER RepoRoot
    仓库根目录，默认当前目录的 git 根。
#>
param(
    [switch] $Fast,
    [switch] $Ci,
    [string] $RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (& git rev-parse --show-toplevel).Trim()
}
Push-Location $RepoRoot

$failures = New-Object System.Collections.Generic.List[string]
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check([string] $Name, [bool] $Ok, [string] $Detail) {
    $checks.Add([pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail })
    if (-not $Ok) { $failures.Add("$Name - $Detail") }
    $tag = 'PASS'
    if (-not $Ok) { $tag = 'FAIL' }
    Write-Output ("[{0}] {1}" -f $tag, $Name)
    if ($Detail) { Write-Output ("        {0}" -f $Detail) }
}

function Get-TrackedFiles([string[]] $Patterns) {
    $args = @('ls-files') + $Patterns
    return @(& git @args)
}

try {
    # ---------------------------------------------------------------- 提交信息
    if ($Ci) {
        $range = ''
        if ($env:GITHUB_BASE_REF) {
            $range = ('origin/{0}..HEAD' -f $env:GITHUB_BASE_REF)
        } else {
            $range = 'HEAD~1..HEAD'
        }
        & pwsh -NoProfile -File (Join-Path $RepoRoot 'scripts/check-commits.ps1') -Range $range -RepoRoot $RepoRoot
        Add-Check '提交信息（Conventional Commits）' ($LASTEXITCODE -eq 0) "范围 $range"
    } else {
        $base = 'origin/master'
        & git rev-parse --verify --quiet $base | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $range = "$base..HEAD"
        } else {
            $range = 'HEAD~10..HEAD'
        }
        & pwsh -NoProfile -File (Join-Path $RepoRoot 'scripts/check-commits.ps1') -Range $range -RepoRoot $RepoRoot
        Add-Check '提交信息（Conventional Commits）' ($LASTEXITCODE -eq 0) "范围 $range"
    }

    # ------------------------------------------------------------- 禁止入库路径
    $forbidden = '^(docs/|works/|AI_test/|thirdParty/|logs/|out/|ISOLATION-VM-SETUP\.md|work\.md|\.orca/)'
    $tracked = Get-TrackedFiles @()
    $badPaths = @($tracked | Where-Object { $_ -match $forbidden })
    Add-Check '禁止入库路径（私有目录/构建产物）' ($badPaths.Count -eq 0) (($badPaths -join ', '))

    # ------------------------------------------------------- 含中文源文件需 BOM
    $sources = Get-TrackedFiles @('*.hpp', '*.cpp', '*.h')
    $missingBom = New-Object System.Collections.Generic.List[string]
    foreach ($f in $sources) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $bytes = [IO.File]::ReadAllBytes((Join-Path $RepoRoot $f))
        $hasNonAscii = $false
        foreach ($b in $bytes) { if ($b -gt 127) { $hasNonAscii = $true; break } }
        if (-not $hasNonAscii) { continue }
        $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
        if (-not $hasBom) { $missingBom.Add($f) }
    }
    Add-Check '含中文本源文件带 UTF-8 BOM' ($missingBom.Count -eq 0) (($missingBom -join ', '))

    # ------------------------------------------------- .cmd/.bat 必须纯 ASCII
    $scripts = Get-TrackedFiles @('*.cmd', '*.bat')
    $nonAscii = New-Object System.Collections.Generic.List[string]
    foreach ($f in $scripts) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $bytes = [IO.File]::ReadAllBytes((Join-Path $RepoRoot $f))
        foreach ($b in $bytes) {
            if ($b -gt 127) { $nonAscii.Add($f); break }
        }
    }
    Add-Check '批处理文件纯 ASCII' ($nonAscii.Count -eq 0) (($nonAscii -join ', '))

    # ------------------------------------------------------------ 公开面自检
    $publicFiles = @(Get-TrackedFiles @('*.hpp', '*.cpp', '*.h', '*.md', '*.toml', '*.json', '*.txt'))
    $pattern = 'docs/[0-9A-Za-z]|\[OPT-|\[MOD-|学习|教学|复盘|学习者|任务卡|自由区|批次|learning'
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($f in $publicFiles) {
        if ($f -eq 'CONTRIBUTING.md') { continue }  # 规范文件本身描述规则，允许出现词表
        $full = Join-Path $RepoRoot $f
        if (-not (Test-Path -LiteralPath $full)) { continue }
        $matches = Select-String -LiteralPath $full -Pattern $pattern -ErrorAction SilentlyContinue
        if ($matches) {
            foreach ($m in $matches) { $hits.Add(('{0}:{1}' -f $f, $m.LineNumber)) }
        }
    }
    Add-Check '公开面纯净（无私有路径/内部代号/教学用语）' ($hits.Count -eq 0) (($hits -join ', '))

    # ------------------------------------------------------------- git 卫生
    & git diff --check | Out-Null
    Add-Check 'git diff --check（行尾空白）' ($LASTEXITCODE -eq 0) ''

    $statusBad = @(& git status --porcelain | Where-Object { $_ -match '^\?\?' -and $_ -notmatch '\.orca/' })
    # 未跟踪文件只提示不失败：可能正是本次要提交的新文件。
    if ($statusBad.Count -gt 0) {
        Write-Output ('[INFO] 未跟踪文件 {0} 个（提交前确认是否需要入库）' -f $statusBad.Count)
    }

    # --------------------------------------------------------------- 构建/回归
    if (-not $Fast) {
        $buildScripts = @(
            @{ Name = 'CMake Debug 构建'; File = 'AI_test\build64.bat' },
            @{ Name = 'CMake Release 构建'; File = 'AI_test\build64_release.bat' },
            @{ Name = 'MSVC Debug 构建'; File = 'AI_test\build64_msvc.bat' },
            @{ Name = '全量回归 ctest'; File = 'AI_test\ctest64.bat' }
        )
        foreach ($b in $buildScripts) {
            $path = Join-Path $RepoRoot $b.File
            if (-not (Test-Path -LiteralPath $path)) {
                Add-Check $b.Name $false ("缺少脚本 " + $b.File)
                continue
            }
            & cmd.exe /d /c "`"$path`"" | Out-Host
            Add-Check $b.Name ($LASTEXITCODE -eq 0) ("exit " + $LASTEXITCODE)
        }
    }

    # ------------------------------------------------------------------- 汇总
    Write-Output ''
    if ($failures.Count -gt 0) {
        Write-Output ("verify: FAILED（{0} 项）" -f $failures.Count)
        foreach ($f in $failures) { Write-Output ("  - {0}" -f $f) }
        exit 1
    }
    Write-Output 'verify: OK'
    exit 0
}
finally {
    Pop-Location
}
