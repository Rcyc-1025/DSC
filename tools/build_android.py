
# -*- coding: utf-8 -*-
"""手工打包 Android APK（不依赖 Gradle）：NDK clang 编译核心层 -> javac -> d8 -> aapt2 -> zipalign -> apksigner。"""
import os, sys, glob, shutil, subprocess, time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NDKBIN = r'D:\Code\android-ndk\android-ndk-r27c\toolchains\llvm\prebuilt\windows-x86_64\bin'
SYSROOT = r'D:\Code\android-ndk\android-ndk-r27c\toolchains\llvm\prebuilt\windows-x86_64\sysroot'
SDK = os.path.expandvars(r'%LOCALAPPDATA%\Android\Sdk')
# build-tools 34 的 d8(R8 8.2) 在 JDK 21 上会抛 NPE：换 37.0.0 的 d8；
# 配套用 android-35 的 android.jar 作为 --lib，targetSdk 仍是 34。
BT = os.path.join(SDK, 'build-tools', '37.0.0')
PLATFORM = os.path.join(SDK, 'platforms', 'android-35', 'android.jar')
JDK = r'D:\Code\JDK\jdk-21.0.12.1\bin'
OUT = os.path.join(ROOT, 'build_android')
# 签名密钥放在仓库外：clean.py 会清掉 build_android/，早先把 keystore 放在其中的做法
# 导致每次打包都重新生成证书，新版本装不上（Android 不允许跨签名覆盖安装，见 0.1.2 说明）。
# 这个密钥必须长期保留并自行备份：换了它，已安装的版本就只能卸载重装。
KEYSTORE = os.path.join(os.path.expandvars(r'%LOCALAPPDATA%'), 'DSC', 'signing', 'dsc-release.keystore')
APP = os.path.join(ROOT, 'android', 'app', 'src', 'main')

CLANG = os.path.join(NDKBIN, 'clang.exe')
TARGET = '--target=aarch64-linux-android24'


def run(cmd, **kw):
    print('>', ' '.join(cmd[:6]), '...' if len(cmd) > 6 else '')
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)
    if p.returncode != 0:
        print('FAILED rc=', p.returncode)
        print(p.stdout[-4000:])
        print(p.stderr[-4000:])
        raise SystemExit(1)
    if p.stdout.strip():
        print(p.stdout.strip()[-2000:])
    if p.stderr.strip():
        print(p.stderr.strip()[-3000:])
    return p


def main():
    for sub in ['obj', 'dex', 'res', 'classes']:
        d = os.path.join(OUT, sub)
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d, exist_ok=True)

    # 1. 核心层 + JNI 桥 -> libdsc.so（arm64-v8a）
    srcs = sorted(glob.glob(os.path.join(ROOT, 'src', 'core', '*.c')))
    # 平台层（transport_android.c 提供 dsc_transport_*，是配对/同步的底座）与
    # Android 的凭证实现（credential_android.c 走 Keystore）。共享库允许未解析符号，
    # 漏掉它们编译不会报错、运行到 dlopen 才失败，所以这里必须显式列上。
    srcs += sorted(glob.glob(os.path.join(ROOT, 'src', 'platform', 'android', '*.c')))
    srcs += [os.path.join(ROOT, 'third_party', 'sqlite', 'sqlite3.c'),
             os.path.join(APP, 'cpp', 'dsc_jni.c'),
             os.path.join(APP, 'cpp', 'credential_android.c')]
    objs = []
    for i, s in enumerate(srcs):
        o = os.path.join(OUT, 'obj', '%02d_%s.o' % (i, os.path.splitext(os.path.basename(s))[0]))
        cmd = [CLANG, TARGET, '-std=c11', '-O2', '-fPIC', '-Wall', '-Wextra',
               '-Wno-unused-parameter', '-Wno-missing-field-initializers',
               '-I', os.path.join(ROOT, 'src', 'core'),
               '-I', os.path.join(ROOT, 'third_party', 'sqlite'),
               '-I', os.path.join(APP, 'cpp'),
               '-DQIC_ANDROID', '-c', s, '-o', o]
        if os.path.basename(s) == 'sqlite3.c':
            cmd = [c for c in cmd if c not in ('-Wall', '-Wextra')]
            cmd += ['-w', '-DSQLITE_THREADSAFE=1', '-DSQLITE_OMIT_LOAD_EXTENSION',
                    '-DSQLITE_DQS=0']
        run(cmd)
        objs.append(o)
    so = os.path.join(OUT, 'libdsc.so')
    # 0.1.1：16KB 页对齐。Android 15/16 起不少新机（含 16KB 内核的旗舰）只接受
    # p_align >= 0x4000 的 .so，4KB 对齐的库会 dlopen 失败 -> UnsatisfiedLinkError -> 一点就退。
    # 16KB 对齐的库在 4KB 内核上同样可用，是官方推荐的双兼容做法。
    # --no-undefined：Android 的 dlopen 会逐个解析符号，缺一个就整个 .so 加载失败
    # （0.1.1 真机上插件符号缺失导致的 UnsatisfiedLinkError 就是这么来的）。
    # 加这个开关后，同样的漏编在链接期直接失败，不会溜进安装包。
    run([CLANG, TARGET, '-shared', '-o', so] + objs +
        ['-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384',
         '-Wl,--no-undefined', '-lm', '-llog'])
    print('libdsc.so', os.path.getsize(so), 'bytes')

    # 2. javac
    java_srcs = glob.glob(os.path.join(APP, 'java', 'com', 'dsc', 'app', '*.java'))
    run([os.path.join(JDK, 'javac.exe'), '-source', '8', '-target', '8', '-encoding', 'UTF-8',
         '-nowarn', '-cp', PLATFORM, '-d', os.path.join(OUT, 'classes')] + java_srcs)

    # 3. d8 -> classes.dex
    #    类文件逐个当参数传给 d8 会撞上 Windows 命令行长度上限（内部类一多就 "The command line is too long."），
    #    先打成 jar 再让 d8 读这一个输入。
    classes_jar = os.path.join(OUT, 'classes.jar')
    if os.path.exists(classes_jar):
        os.remove(classes_jar)
    run([os.path.join(JDK, 'jar.exe'), 'cf', classes_jar, '-C', os.path.join(OUT, 'classes'), '.'])
    run([os.path.join(BT, 'd8.bat'), '--min-api', '24', '--lib', PLATFORM,
         '--output', os.path.join(OUT, 'dex'), classes_jar])

    # 4. aapt2 compile + link
    res_dir = os.path.join(APP, 'res')
    flat_zip = os.path.join(OUT, 'res', 'res.zip')
    run([os.path.join(BT, 'aapt2.exe'), 'compile', '--dir', res_dir, '-o', flat_zip])
    base_apk = os.path.join(OUT, 'base.apk')
    manifest = os.path.join(APP, 'AndroidManifest.xml')
    run([os.path.join(BT, 'aapt2.exe'), 'link', '-o', base_apk,
         '-I', PLATFORM, '--manifest', manifest, '--min-sdk-version', '24',
         '--target-sdk-version', '34', '--version-code', '9', '--version-name', '0.1.4',
         '--auto-add-overlay', flat_zip])

    # 5. 塞入 classes.dex 与 lib/arm64-v8a/libdsc.so
    import zipfile
    with zipfile.ZipFile(base_apk, 'a', zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(OUT, 'dex', 'classes.dex'), 'classes.dex')
        z.write(so, 'lib/arm64-v8a/libdsc.so')

    # 6. zipalign
    aligned = os.path.join(OUT, 'aligned.apk')
    run([os.path.join(BT, 'zipalign.exe'), '-f', '-p', '4', base_apk, aligned])

    # 7. 自签名证书
    ks = KEYSTORE
    os.makedirs(os.path.dirname(ks), exist_ok=True)
    if not os.path.exists(ks):
        run([os.path.join(JDK, 'keytool.exe'), '-genkeypair', '-keystore', ks,
             '-alias', 'dsc', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
             '-storepass', 'dsc123', '-keypass', 'dsc123',
             '-dname', 'CN=DSC, OU=Dev, O=DSC, L=Local, ST=Local, C=CN'])
        print('新签名密钥已生成：%s' % ks)
    print('签名密钥：%s（仓库外，自行备份；换掉它会导致已安装版本无法覆盖升级）' % ks)

    # 8. apksigner 签名
    signed = os.path.join(OUT, 'DSC.apk')
    if os.path.exists(signed):
        os.remove(signed)
    run([os.path.join(BT, 'apksigner.bat'), 'sign', '--ks', ks, '--ks-key-alias', 'dsc',
         '--ks-pass', 'pass:dsc123', '--key-pass', 'pass:dsc123',
         '--out', signed, aligned])
    run([os.path.join(BT, 'apksigner.bat'), 'verify', '--verbose', signed])
    print('APK', signed, os.path.getsize(signed), 'bytes')


if __name__ == '__main__':
    t0 = time.time()
    main()
    print('done in %.1fs' % (time.time() - t0))
