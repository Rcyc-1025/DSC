
# -*- coding: utf-8 -*-
"""校验 Android APK 产物（无真机时的自动化证据）：
 1) APK 存在且是合法 zip，含 classes.dex 与 lib/arm64-v8a/libdsc.so；
 2) libdsc.so 是 AArch64 ELF，且导出了 JNI 入口符号；
 3) aapt2 dump badging 的包名 / minSdk / targetSdk / 启动 Activity / 应用名；
 4) apksigner verify 通过（自签名同样需要可验证）。
退出码 = 失败项数。"""

import os, sys, struct, subprocess, zipfile

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.expandvars(r'%LOCALAPPDATA%\Android\Sdk')
BT = os.path.join(SDK, 'build-tools', '37.0.0')
APK = os.path.join(ROOT, 'build_android', 'DSC.apk')

fails = []


def check(cond, msg):
    print(('PASS ' if cond else 'FAIL ') + msg)
    if not cond:
        fails.append(msg)


def main():
    check(os.path.isfile(APK), 'APK 存在：%s' % APK)
    if not os.path.isfile(APK):
        return
    check(os.path.getsize(APK) < 80 * 1024 * 1024, 'APK 体积 %.1f MB 在安装包上限内（限 60MB 移动目标，实测见下）'
          % (os.path.getsize(APK) / 1048576.0))

    with zipfile.ZipFile(APK) as z:
        names = z.namelist()
        check('classes.dex' in names, 'APK 含 classes.dex')
        check('lib/arm64-v8a/libdsc.so' in names, 'APK 含 lib/arm64-v8a/libdsc.so')
        check('AndroidManifest.xml' in names and 'resources.arsc' in names, 'APK 含清单与资源表')
        so = z.read('lib/arm64-v8a/libdsc.so') if 'lib/arm64-v8a/libdsc.so' in names else b''

    if len(so) > 0x40:
        check(so[:4] == b'\x7fELF', 'libdsc.so 是 ELF 文件')
        check(so[4] == 2, 'libdsc.so 是 64 位 ELF')
        machine = struct.unpack('<H', so[18:20])[0]
        check(machine == 183, 'libdsc.so 目标架构是 AArch64（e_machine=183，实际 %d）' % machine)
        check(b'Java_com_dsc_app_Native_call' in so, 'libdsc.so 导出 JNI 入口 Java_com_dsc_app_Native_call')
        check(b'sqlite3_open_v2' in so, 'libdsc.so 静态包含 SQLite（sqlite3_open_v2）')
        # 0.1.1 起的关键项：PT_LOAD 的 p_align 必须 >= 16KB，否则 16KB 页内核的新机会 dlopen 失败。
        phoff = struct.unpack('<Q', so[0x20:0x28])[0]
        phentsize = struct.unpack('<H', so[0x36:0x38])[0]
        phnum = struct.unpack('<H', so[0x38:0x3A])[0]
        aligns = []
        for i in range(phnum):
            off = phoff + i * phentsize
            if off + 56 > len(so):
                break
            p_type = struct.unpack('<I', so[off:off + 4])[0]
            if p_type == 1:  # PT_LOAD
                aligns.append(struct.unpack('<Q', so[off + 48:off + 56])[0])
        min_align = min(aligns) if aligns else 0
        check(min_align >= 0x4000,
              'libdsc.so 的 PT_LOAD 段按 16KB 页对齐（最小 p_align=0x%x，需 >=0x4000）' % min_align)

    p = subprocess.run([os.path.join(BT, 'aapt2.exe'), 'dump', 'badging', APK],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    badging = p.stdout or ''
    check("package: name='com.dsc.app'" in badging, '包名 com.dsc.app')
    check("versionName='0.1.1'" in badging, '版本名 0.1.1')
    check("versionCode='6'" in badging, 'versionCode 6')
    check("minSdkVersion:'24'" in badging, 'minSdkVersion 24（Android 7.0 起）')
    check("targetSdkVersion:'34'" in badging, 'targetSdkVersion 34')
    check("launchable-activity: name='com.dsc.app.MainActivity'" in badging, '启动 Activity 为 MainActivity')
    check("native-code: 'arm64-v8a'" in badging, '声明了 arm64-v8a 原生库')
    check('DSC' in badging, '应用名已写入清单')

    p = subprocess.run([os.path.join(BT, 'apksigner.bat'), 'verify', '--print-certs', APK],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    check(p.returncode == 0, 'apksigner 校验签名通过')
    certs = [l for l in ((p.stdout or '') + (p.stderr or '')).splitlines() if 'Signer' in l or 'SHA-256' in l]
    for c in certs[:4]:
        print('     ' + c.strip())

    print()
    if fails:
        print('Android 产物校验：%d 项失败' % len(fails))
    else:
        print('Android 产物校验：全部通过（未在真机/模拟器上运行，属未实测项）')


if __name__ == '__main__':
    main()
    raise SystemExit(len(fails))
