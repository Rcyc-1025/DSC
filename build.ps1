# build.ps1 —— DSC 构建脚本（Windows，mingw-w64 + 纯 C11）
# 用法： pwsh -File build.ps1 -Target test
param(
    [string]$Target = 'test'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
# 优先用本机固定位置的 MSYS2 ucrt64；找不到就退回 PATH 里的 gcc（CI 里由 setup-msys2 提供）。
$Mingw = 'D:\Code\msys2ucrt\ucrt64\bin'
if (Test-Path $Mingw) { $env:PATH = $Mingw + ';' + $env:PATH }
$env:TMP = Join-Path $Root 'build\tmp'
$env:TEMP = $env:TMP
New-Item -ItemType Directory -Force -Path $env:TMP | Out-Null
$Obj = Join-Path $Root 'build\obj'
New-Item -ItemType Directory -Force -Path $Obj | Out-Null
$Gcc = if (Test-Path (Join-Path $Mingw 'gcc.exe')) { Join-Path $Mingw 'gcc.exe' } else { 'gcc.exe' }

$Inc = @('-I', (Join-Path $Root 'src\core'), '-I', (Join-Path $Root 'src\platform\win'), '-I', (Join-Path $Root 'third_party\sqlite'))
$Cflags = @('-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers')
$SqliteFlags = @('-std=c11', '-O2', '-w', '-DSQLITE_THREADSAFE=1', '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_DQS=0', '-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1')

function Invoke-Gcc {
    param([string[]]$Arguments, [string]$What)
    if (-not $Arguments -or $Arguments.Count -eq 0) { Write-Host ('[FAIL] empty argument list for ' + $What); exit 1 }
    $errFile = Join-Path $Root 'build\last_error.txt'
    $outFile = Join-Path $Root 'build\last_out.txt'
    $p = Start-Process -FilePath $Gcc -ArgumentList $Arguments -NoNewWindow -Wait -PassThru -RedirectStandardError $errFile -RedirectStandardOutput $outFile
    if ($p.ExitCode -ne 0) {
        Write-Host ('[FAIL] ' + $What)
        Get-Content $errFile -Raw
        exit 1
    }
    $warn = Get-Content $errFile -Raw
    if ($warn) { Write-Host ('[warn] ' + $What); Write-Host $warn }
}

function Build-Object {
    param([string]$Source, [string[]]$Extra)
    $name = [System.IO.Path]::GetFileNameWithoutExtension($Source)
    $obj = Join-Path $Obj ($name + '.o')
    $srcTime = (Get-Item $Source).LastWriteTime
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge $srcTime)) { return $obj }
    $a = $Cflags + $Inc + $Extra + @('-c', $Source, '-o', $obj)
    Invoke-Gcc -Arguments $a -What $name
    return $obj
}

# sqlite3 单独编译（只编一次）
$sqliteObj = Join-Path $Obj 'sqlite3.o'
$sqliteSrc = Join-Path $Root 'third_party\sqlite\sqlite3.c'
if (-not (Test-Path $sqliteObj) -or ((Get-Item $sqliteObj).LastWriteTime -lt (Get-Item $sqliteSrc).LastWriteTime)) {
    $a = $SqliteFlags + @('-c', $sqliteSrc, '-o', $sqliteObj)
    Invoke-Gcc -Arguments $a -What 'sqlite3'
}

$coreObjs = @()
foreach ($f in (Get-ChildItem (Join-Path $Root 'src\core\*.c'))) {
    $coreObjs += (Build-Object -Source $f.FullName -Extra @())
}

$winObjs = @()
foreach ($f in (Get-ChildItem (Join-Path $Root 'src\platform\win\*.c') -ErrorAction SilentlyContinue)) {
    $winObjs += (Build-Object -Source $f.FullName -Extra @('-DUNICODE', '-D_UNICODE'))
}

function Build-Dll {
    param([string]$Source, [string]$Out, [string[]]$Include)
    $a = @('-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-shared') + $Include + @($Source, '-o', $Out)
    Invoke-Gcc -Arguments $a -What ([System.IO.Path]::GetFileName($Out))
}

function Link-Exe {
    param([string]$Out, [string[]]$Objects, [string[]]$Libs)
    $a = @('-o', $Out) + $Objects + @($sqliteObj) + $Libs
    Invoke-Gcc -Arguments $a -What ([System.IO.Path]::GetFileName($Out))
}

switch ($Target) {
    'test' {
        $t = Build-Object -Source (Join-Path $Root 'tests\unit\test_core.c') -Extra @()
        Link-Exe -Out (Join-Path $Root 'build\test_core.exe') -Objects ($coreObjs + $winObjs + @($t)) -Libs @('-lws2_32', '-lbthprops', '-lbluetoothapis', '-ladvapi32', '-lwinhttp', '-lm', '-lpsapi', '-lgdiplus', '-lshell32', '-lgdi32', '-luser32')
        Write-Host 'built: build\test_core.exe'

        # 插件宿主进程与三个测试插件（正常 / 崩溃 / 卡死）
        $plgInc = @('-I', (Join-Path $Root 'src\plugin_runner'))
        $hostObj = Build-Object -Source (Join-Path $Root 'src\plugin_runner\dsc_plugin_host.c') -Extra $plgInc
        $hostExe = Join-Path $Root 'build\dsc_plugin_host.exe'
        Invoke-Gcc -Arguments @('-o', $hostExe, $hostObj, '-static-libgcc') -What 'dsc_plugin_host.exe'
        Write-Host 'built: build\dsc_plugin_host.exe'

        $pluginsOut = Join-Path $Root 'build\plugins'
        New-Item -ItemType Directory -Force -Path $pluginsOut | Out-Null
        foreach ($f in (Get-ChildItem (Join-Path $Root 'tests\plugins\*.c') -ErrorAction SilentlyContinue)) {
            $name = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
            Build-Dll -Source $f.FullName -Out (Join-Path $pluginsOut ($name + '.dll')) -Include $plgInc
        }
        Write-Host 'built: build\plugins\*.dll'
    }
    'cli' {
        $cliInc = @(('-I' + (Join-Path $Root 'src\cli')))
        $c = Build-Object -Source (Join-Path $Root 'src\cli\dsc_cli.c') -Extra $cliInc
        $v = Build-Object -Source (Join-Path $Root 'src\cli\dsc_verify.c') -Extra $cliInc
        Link-Exe -Out (Join-Path $Root 'build\dsc-cli.exe') -Objects ($coreObjs + $winObjs + @($c, $v)) -Libs @('-lws2_32', '-lbthprops', '-lbluetoothapis', '-ladvapi32', '-lwinhttp', '-lm', '-lpsapi', '-lgdiplus', '-lgdi32', '-luser32', '-lshell32', '-lcomctl32')
        Write-Host 'built: build\dsc-cli.exe'
    }
    'gui' {
        $g = Build-Object -Source (Join-Path $Root 'src\ui\win\dsc_app.c') -Extra @('-DUNICODE', '-D_UNICODE')
        $exe = Join-Path $Root 'build\DSC.exe'
        $a = @('-mwindows', '-o', $exe, $g) + $coreObjs + $winObjs + @($sqliteObj) + @('-lws2_32', '-lbthprops', '-lbluetoothapis', '-ladvapi32', '-lwinhttp', '-lm', '-lpsapi', '-lgdi32', '-luser32', '-lshell32', '-lcomctl32', '-lcomdlg32', '-lgdiplus', '-lmsimg32', '-static-libgcc')
        Invoke-Gcc -Arguments $a -What 'DSC.exe'
        Write-Host 'built: build\DSC.exe'
    }
    'guidiag' {
        $g = Build-Object -Source (Join-Path $Root 'src\ui\win\dsc_app.c') -Extra @('-DUNICODE', '-D_UNICODE')
        $exe = Join-Path $Root 'build\DscDiag.exe'
        $a = @('-o', $exe, $g) + $coreObjs + $winObjs + @($sqliteObj) + @('-lws2_32', '-lbthprops', '-lbluetoothapis', '-ladvapi32', '-lwinhttp', '-lm', '-lpsapi', '-lgdi32', '-luser32', '-lshell32', '-lcomctl32', '-lcomdlg32', '-lgdiplus', '-lmsimg32', '-static-libgcc')
        Invoke-Gcc -Arguments $a -What 'DscDiag.exe'
        Write-Host 'built: build\DscDiag.exe'
    }
    default { Write-Host ('unknown target: ' + $Target); exit 2 }
}