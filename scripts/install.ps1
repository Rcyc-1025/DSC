# install.ps1 —— 把 AIChat 装到当前用户的程序目录并创建开始菜单快捷方式（不需要管理员）。
# 用法：右键 → 使用 PowerShell 运行；或 powershell -ExecutionPolicy Bypass -File install.ps1
param(
    [string]$Target = (Join-Path $env:LOCALAPPDATA 'Programs\AIChat')
)

$ErrorActionPreference = 'Stop'
$src = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "安装到 $Target"
New-Item -ItemType Directory -Force -Path $Target | Out-Null
Copy-Item -Path (Join-Path $src '*') -Destination $Target -Recurse -Force

$exe = Join-Path $Target 'AIChat.exe'
$menu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
New-Item -ItemType Directory -Force -Path $menu | Out-Null
$lnk = Join-Path $menu 'AIChat 陪伴对话.lnk'
$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($lnk)
$sc.TargetPath = $exe
$sc.WorkingDirectory = $Target
$sc.IconLocation = $exe
$sc.Description = 'DeepSeek 陪伴对话软件'
$sc.Save()

Write-Host "已创建快捷方式：$lnk"
Write-Host '卸载：删除该快捷方式与安装目录即可（用户数据在 %LOCALAPPDATA%\AIChat，默认保留）。'
