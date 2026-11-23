#!/usr/bin/env python3
"""用发布方自己的 keystore 给 APK 签名（技术设计 16.1：安装包签名）。

不签发证书：keystore 由发布方提供，路径用环境变量 SAI_ANDROID_KEYSTORE 指定，
口令用 SAI_ANDROID_STOREPASS / SAI_ANDROID_KEYPASS，别名用 SAI_ANDROID_ALIAS。
缺任何一项都明确报错并以退出码 3 结束，绝不回退到仓库里的自签调试证书。

用法：
    python tools/sign_android.py [--in build_android/Sai.apk] [--out dist/Sai.apk]
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")
BT = os.path.join(SDK, "build-tools", "37.0.0")
APKSIGNER = os.path.join(BT, "apksigner.bat")
ZIPALIGN = os.path.join(BT, "zipalign.exe")
JDK = r"D:\Code\JDK\jdk-21.0.12.1\bin"
KEYTOOL = os.path.join(JDK, "keytool.exe")


def fail(msg, code=1):
    print("FAILED " + msg)
    sys.exit(code)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", default=os.path.join(ROOT, "build_android", "Sai.apk"))
    ap.add_argument("--out", dest="dst", default=os.path.join(ROOT, "dist", "Sai.apk"))
    args = ap.parse_args()

    store = os.environ.get("SAI_ANDROID_KEYSTORE", "")
    storepass = os.environ.get("SAI_ANDROID_STOREPASS", "")
    keypass = os.environ.get("SAI_ANDROID_KEYPASS", "") or storepass
    alias = os.environ.get("SAI_ANDROID_ALIAS", "")
    missing = [n for n, v in (("SAI_ANDROID_KEYSTORE", store), ("SAI_ANDROID_STOREPASS", storepass),
                              ("SAI_ANDROID_ALIAS", alias)) if not v]
    if missing:
        fail("缺少发布方 keystore 配置：" + "、".join(missing) +
             "。本仓库不签发证书；请提供发布方自己的 keystore（含口令与别名）后重试。", 3)
    if not os.path.isfile(store):
        fail("keystore 不存在：" + store, 3)
    for exe in (APKSIGNER, ZIPALIGN):
        if not os.path.isfile(exe):
            fail("缺少 Android 构建工具：" + exe + "（需要 build-tools 37.0.0）", 2)
    if not os.path.isfile(args.src):
        fail("APK 不存在：" + args.src + "（先运行 tools/build_android.py）", 4)

    os.makedirs(os.path.dirname(args.dst), exist_ok=True)
    aligned = os.path.join(os.path.dirname(args.dst), "_aligned.apk")
    r = subprocess.run([ZIPALIGN, "-f", "-p", "4", args.src, aligned], capture_output=True)
    if r.returncode != 0:
        fail("zipalign 失败：" + r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace"))
    r = subprocess.run([APKSIGNER, "sign", "--ks", store, "--ks-pass", "pass:" + storepass,
                        "--ks-key-alias", alias, "--key-pass", "pass:" + keypass,
                        "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
                        "--v3-signing-enabled", "true", "--out", args.dst, aligned],
                       capture_output=True)
    if r.returncode != 0:
        fail("apksigner 签名失败：" + r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace"))
    try:
        os.remove(aligned)
    except OSError:
        pass
    r = subprocess.run([APKSIGNER, "verify", "--verbose", "--print-certs", args.dst], capture_output=True)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    if r.returncode != 0:
        fail("apksigner 验签失败：" + out)
    print(out.strip())
    print("已用发布方证书签名：" + args.dst + "（" + str(os.path.getsize(args.dst)) + " 字节）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
