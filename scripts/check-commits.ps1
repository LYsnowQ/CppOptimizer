#Requires -Version 5.1
<#
.SYNOPSIS
    Conventional Commits 校验（本地钩子与 CI 共用）。

.DESCRIPTION
    逐条检查提交首行是否符合 `<type>(<scope>): <中文简述>`：
      - type 必须在白名单内；
      - scope 必须是小写字母/数字/下划线，且在本项目词表内（可用 -AllowAnyScope 放宽）；
      - 首行不超过 72 个字符（按字符计，不按字节）；
      - 禁止 WIP / fixup! / squash! / 无信息量消息。

.PARAMETER Range
    git 的提交范围，如 origin/master..HEAD。省略时检查最近 20 条。

.PARAMETER RepoRoot
    仓库根目录。默认取当前目录的 git 根。

.PARAMETER AllowAnyScope
    只校验格式，不校验 scope 词表（分支保护外的临时检查用）。
#>
param(
    [string] $Range = '',
    [string] $RepoRoot = '',
    [switch] $AllowAnyScope
)

$ErrorActionPreference = 'Stop'

$types = @('feat', 'fix', 'refactor', 'perf', 'docs', 'test', 'build', 'ci', 'chore', 'style')
$scopes = @(
    'power', 'memory', 'policy', 'priority',
    'ipc', 'service', 'activity', 'metrics', 'process', 'config', 'logger', 'platform', 'audit',
    'build', 'docs', 'tooling', 'repo'
)

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (& git rev-parse --show-toplevel).Trim()
}
if ([string]::IsNullOrWhiteSpace($Range)) {
    $Range = 'HEAD~20..HEAD'
}

Push-Location $RepoRoot
try {
    $hasRange = $true
    & git rev-parse --verify --quiet ($Range.Split('..')[0]) | Out-Null
    if ($LASTEXITCODE -ne 0) { $hasRange = $false }

    if ($hasRange) {
        $log = & git log --format='%H%x09%s' $Range
    } else {
        $log = & git log --format='%H%x09%s' -n 20
    }
    if (-not $log) {
        Write-Output 'check-commits: no commits to check'
        exit 0
    }

    $typePattern = ($types -join '|')
    $scopePattern = '[a-z0-9_]+'
    $pattern = '^(?<type>' + $typePattern + ')(\((?<scope>' + $scopePattern + ')\))?!?: .+$'

    $failures = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]

    foreach ($line in $log) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line.Split("`t", 2)
        $sha = $parts[0]
        $subject = ''
        if ($parts.Length -gt 1) { $subject = $parts[1] }
        $short = $sha.Substring(0, [Math]::Min(8, $sha.Length))

        # 合并提交与回滚提交放行（Git 自动生成的消息不归作者控制）。
        if ($subject -like 'Merge *' -or $subject -like 'Revert *') { continue }

        if ($subject -match '^(WIP|wip|fixup!|squash!|tmp|temp)\b') {
            $failures.Add("$short  禁止的提交类型（WIP/fixup/squash）: $subject")
            continue
        }

        if (-not ($subject -match $pattern)) {
            $failures.Add("$short  格式不符 <type>(<scope>): <简述>: $subject")
            continue
        }

        $type = $Matches['type']
        $scope = $Matches['scope']
        $length = $subject.Length
        if ($length -gt 72) {
            $failures.Add("$short  首行超过 72 字符（$length）: $subject")
        }
        if ($type -in @('style') -and $subject -match 'feat|新增') {
            $warnings.Add("$short  style 不应承载功能改动: $subject")
        }
        if (-not $AllowAnyScope -and $scope -and ($scopes -notcontains $scope)) {
            $failures.Add("$short  未登记的 scope '$scope'（可用: $($scopes -join ', ')）: $subject")
        }
    }

    foreach ($w in $warnings) { Write-Output "WARN  $w" }
    if ($failures.Count -gt 0) {
        Write-Output ''
        foreach ($f in $failures) { Write-Output "FAIL  $f" }
        Write-Output ''
        Write-Output "check-commits: $($failures.Count) 条提交不符合规范（范围 $Range）"
        exit 1
    }
    Write-Output "check-commits: OK（范围 $Range）"
    exit 0
}
finally {
    Pop-Location
}
