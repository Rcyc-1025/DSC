#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""把 DSC 便携目录打成一个自解压安装包（纯 C 安装程序 + 内嵌 payload）。

用法：
    python tools\make_installer.py [版本号]

前置：先跑 build.ps1 -Target gui 与 python tools\package_win.py（便携目录在 dist\ 下）。
产物：dist\DSC-<版本>-win-x64-setup.exe 与其 .sha256。
"""
import hashlib
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MINGW = r"D:\Code\msys2ucrt\ucrt64\bin"
MARKER = b"AICSETUP-PAYLOAD-V1"
VERSION = sys.argv[1] if len(sys.argv) > 1 else "0.1.0"

GCC = os.path.join(MINGW, "gcc.exe")
CFLAGS = ["-std=c11", "-O2", "-Wall", "-Wextra", "-municode", "-DUNICODE", "-D_UNICODE", "-mwindows"]
LIBS = ["-lole32", "-luuid", "-lshell32", "-lshlwapi", "-luser32", "-lgdi32", "-static-libgcc"]


def build(name, src):
    out = os.path.join(ROOT, "build", "installer", name)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = MINGW + ";" + env.get("PATH", "")
    cmd = [GCC] + CFLAGS + [src, "-o", out] + LIBS
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    if r.returncode != 0:
        print("编译失败：" + name)
        print(r.stderr)
        raise SystemExit(1)
    if r.stderr.strip():
        print("[告警] " + name)
        print(r.stderr.strip())
    return out


def collect(dist_dir):
    entries = []
    for dirpath, _dirs, files in os.walk(dist_dir):
        for f in sorted(files):
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, dist_dir).replace("\\", "/")
            with open(full, "rb") as fh:
                entries.append((rel, fh.read()))
    entries.sort(key=lambda e: e[0])
    return entries


def main():
    dist_dir = os.path.join(ROOT, "dist", "DSC-%s-win-x64" % VERSION)
    if not os.path.isdir(dist_dir):
        print("找不到便携目录：%s（先跑 python tools\\package_win.py）" % dist_dir)
        raise SystemExit(2)

    setup_exe = build("dsc_setup.exe", r"src\installer\dsc_setup.c")
    uninst_exe = build("dsc_uninstall.exe", r"src\installer\dsc_uninstall.c")

    entries = collect(dist_dir)
    with open(uninst_exe, "rb") as fh:
        entries.append(("uninstall.exe", fh.read()))

    with open(setup_exe, "rb") as fh:
        blob = bytearray(fh.read())

    blob += MARKER
    blob += struct.pack("<I", len(entries))
    for rel, data in entries:
        rp = rel.encode("utf-8")
        blob += struct.pack("<I", len(rp))
        blob += rp
        blob += struct.pack("<Q", len(data))
        blob += data

    out = os.path.join(ROOT, "dist", "DSC-%s-win-x64-setup.exe" % VERSION)
    with open(out, "wb") as fh:
        fh.write(blob)

    digest = hashlib.sha256(blob).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8", newline="\n") as fh:
        fh.write("%s  %s\n" % (digest, os.path.basename(out)))

    print("安装程序  %s（%.1f MB，%d 个文件）" % (out, len(blob) / 1024.0 / 1024.0, len(entries)))
    print("SHA-256  %s" % digest)
    print("校验文件 %s.sha256" % out)


if __name__ == "__main__":
    main()
