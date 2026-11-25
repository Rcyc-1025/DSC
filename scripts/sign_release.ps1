# sign_release.ps1 —— 用 signtool 给发布产物签名（需要 Windows SDK 的 signtool.exe + 一份代码签名证书）。
# 本仓库不签发证书：证书由你把 .pfx 放进 dist\signing\（或用 -Pfx 指定），密码用环境变量 DSC_SIGN_PASSWORD 传入。
# 没有 signtool 或没有证书时，脚本明确报错并退出 2/3，产物保持未签名 —— 不做假签名、不自造证书。
param(
    [string]$Pfx = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\dist\signing\dsc.pfx'),
    [string]$Password = $env:DSC_SIGN_PASSWORD,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string]$Thumbprint = '',
    [string[]]$Files = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# 1) 签名工具
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

# 2) 证书来源：-Pfx 文件，或 -Thumbprint（证书已装进当前用户的个人存储）
$usePfx = $false
if ($Pfx -and (Test-Path $Pfx)) {
    if (-not $Password) {
        Write-Host ('找到证书 ' + $Pfx + '，但没有提供密码：请设置环境变量 DSC_SIGN_PASSWORD 后再运行。')
        exit 3
    }
    $usePfx = $true
} elseif ($Thumbprint) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
    if (-not $cert) {
        Write-Host ('证书存储里找不到指纹 ' + $Thumbprint + ' 的证书。请先把 .pfx 导入当前用户的个人存储。')
        exit 3
    }
} else {
    Write-Host ('没有可用的代码签名证书：既没有 ' + $Pfx + '，也没有 -Thumbprint。' + [Environment]::NewLine +
               '本仓库不签发证书；请把供应商提供的 .pfx 放到上述路径（或导入证书存储）后重试。')
    exit 3
}

if ($Files.Count -eq 0) {
    $Files = @(
        (Join-Path $root 'build\DSC.exe'),
        (Join-Path $root 'build\dsc-cli.exe'),
        (Join-Path $root 'build\dsc_plugin_host.exe'),
        (Join-Path $root 'dist\DSC-0.1.4-win-x64-setup.exe')
    ) | Where-Object { Test-Path $_ }
}
if ($Files.Count -eq 0) {
    Write-Host '没有可签名的文件：请先运行 tools\package_win.py 与 tools\make_installer.py 产出产物。'
    exit 4
}

# 3) 签名 + 验签
foreach ($f in $Files) {
    Write-Host ('签名 ' + $f)
    if ($usePfx) {
        & $signtool sign /fd SHA256 /f $Pfx /p $Password /tr $TimestampUrl /td SHA256 $f
    } else {
        & $signtool sign /fd SHA256 /sha1 $Thumbprint /tr $TimestampUrl /td SHA256 $f
    }
    if ($LASTEXITCODE -ne 0) { Write-Host ('签名失败：' + $f + '（signtool 退出码 ' + $LASTEXITCODE + '）'); exit 1 }
    & $signtool verify /pa /v $f
    if ($LASTEXITCODE -ne 0) { Write-Host ('验签失败：' + $f); exit 1 }
}
Write-Host '全部签名并验签通过。'
