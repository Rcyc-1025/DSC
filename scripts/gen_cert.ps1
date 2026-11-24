# gen_cert.ps1 —— 生成自签名代码签名证书（当前用户证书库，不需要管理员）。
# 导出 dist\signing\dsc-selfsigned.cer（公钥）与 dsc-selfsigned.pfx（私钥，密码自己保管）。
param(
    [string]$OutDir = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\dist\signing'),
    [string]$Subject = 'CN=DSC Self-Signed, O=DSC, C=CN',
    [string]$Password = 'dsc123'
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
    -CertStoreLocation 'Cert:\CurrentUser\My' -KeyUsage DigitalSignature `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
    -NotAfter (Get-Date).AddYears(10)

$cer = Join-Path $OutDir 'dsc-selfsigned.cer'
$pfx = Join-Path $OutDir 'dsc-selfsigned.pfx'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
$sec = ConvertTo-SecureString -String $Password -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $sec | Out-Null

Write-Host ('证书指纹：' + $cert.Thumbprint)
Write-Host ('公钥证书：' + $cer)
Write-Host ('私钥包  ：' + $pfx + '（密码 ' + $Password + '，请自行保管）')
Write-Host ''
Write-Host '把公钥证书装进"受信任的根证书颁发机构"后，签名过的 exe 在本机不再提示未知发布者：'
Write-Host ('  Import-Certificate -FilePath ''' + $cer + ''' -CertStoreLocation ''Cert:\CurrentUser\Root''')
