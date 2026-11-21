# sign_release.ps1 —— 用 signtool 给发布产物签名（需要 Windows SDK 的 signtool.exe）。
# 本机若没有 signtool，脚本会明确报"未找到 signtool"并退出 2，不做假签名。
param(
    [string]$Pfx = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\dist\signing\aichat-selfsigned.pfx'),
    [string]$Password = 'aichat123',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string[]]$Files = @()
)

$ErrorActionPreference = 'Stop'

$signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if (-not $signtool) {
    $cand = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($cand) { $signtool = $cand.FullName }
}
if (-not $signtool) {
    Write-Host '未找到 signtool.exe（需要安装 Windows SDK 的签名工具）。产物保持未签名状态，不做假签名。'
    exit 2
}

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Files.Count -eq 0) {
    $Files = @(
        (Join-Path $root 'build\AIChat.exe'),
        (Join-Path $root 'build\aic.exe'),
        (Join-Path $root 'build\aic_plugin_host.exe')
    ) | Where-Object { Test-Path $_ }
}

foreach ($f in $Files) {
    Write-Host ('签名 ' + $f)
    & $signtool sign /fd SHA256 /f $Pfx /p $Password /tr $TimestampUrl /td SHA256 $f
    if ($LASTEXITCODE -ne 0) { Write-Error ('签名失败：' + $f + '（signtool 退出码 ' + $LASTEXITCODE + '）'); exit 1 }
    & $signtool verify /pa $f
    if ($LASTEXITCODE -ne 0) { Write-Error ('验签失败：' + $f); exit 1 }
}
Write-Host '全部签名并验签通过。'
