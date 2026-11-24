#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""build_arm64.py —— 交叉编译 Windows ARM64 (aarch64-w64-windows-gnu) 产物。

工具链不下载任何东西，全部用本机已有组件：
  - 编译器/链接器驱动：本机 LLVM 的 clang.exe（-target aarch64-w64-windows-gnu，-fuse-ld=lld）
  - sysroot：MSYS2 clangarm64 的 aarch64 mingw-w64 包（crt / headers / winpthreads /
    compiler-rt / windows-default-manifest），解压到 DSC_ARM64_SYSROOT（默认 D:\Code\msys2arm64\clangarm64）
  - builtins：sysroot 里的 lib\clang\23\lib\windows\libclang_rt.builtins-aarch64.a，
    复制成 build\arm64\res\lib\aarch64-w64-windows-gnu\libclang_rt.builtins.a
    （clang 按 <resource-dir>/lib/<target>/libclang_rt.builtins.a 找它，所以自带一份 resource-dir）

用法：
  python tools\build_arm64.py
环境变量：
  DSC_LLVM_BIN      clang.exe 所在目录（默认 D:\Code\llvm\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin）
  DSC_ARM64_SYSROOT aarch64 mingw-w64 sysroot 根（默认 D:\Code\msys2arm64\clangarm64）
"""
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLVM_BIN = os.environ.get("DSC_LLVM_BIN", r"D:\Code\llvm\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin")
SYSROOT = os.environ.get("DSC_ARM64_SYSROOT", r"D:\Code\msys2arm64\clangarm64")
CLANG = os.path.join(LLVM_BIN, "clang.exe")
BUILD = os.path.join(ROOT, "build", "arm64")
OBJ = os.path.join(BUILD, "obj")
RES = os.path.join(BUILD, "res")
TARGET = "aarch64-w64-windows-gnu"

BASE = [
    "--target=" + TARGET,
    "--sysroot=" + SYSROOT,
    "-resource-dir=" + RES,
    "-rtlib=compiler-rt",
    "-O2",
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-missing-field-initializers",
    "-I" + os.path.join(ROOT, "src", "core"),
    "-I" + os.path.join(ROOT, "src", "platform", "win"),
    "-I" + os.path.join(ROOT, "third_party", "sqlite"),
]
LIBS = ["-lkernel32", "-luser32", "-lgdi32", "-ladvapi32", "-lshell32", "-lcomctl32",
        "-lcomdlg32", "-lole32", "-luuid", "-lshlwapi", "-lws2_32", "-lwshbth", "-lbthprops", "-lpsapi",
        "-lwinhttp", "-lgdiplus"]


def die(msg):
    print("错误：" + msg)
    sys.exit(1)


def run(cmd, what):
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("失败：" + what)
        print((r.stdout or "")[-2000:])
        print((r.stderr or "")[-3000:])
        die(what + "（exit %d）" % r.returncode)
    return r


def prepare_resource_dir():
    """clang 需要 <resource-dir>/include 与 <resource-dir>/lib/<target>/libclang_rt.builtins.a。"""
    if not os.path.exists(CLANG):
        die("找不到 clang.exe：" + CLANG)
    if not os.path.isdir(SYSROOT):
        die("找不到 aarch64 sysroot：" + SYSROOT + "（先解压 MSYS2 clangarm64 的 crt/headers/compiler-rt 包）")
    src_res = os.path.join(os.path.dirname(os.path.dirname(CLANG)), "lib", "clang")
    versions = sorted(glob.glob(os.path.join(src_res, "*")))
    if not versions:
        die("找不到 clang 自带的 resource-dir")
    src_inc = os.path.join(versions[-1], "include")
    if not os.path.isdir(os.path.join(RES, "include")):
        os.makedirs(RES, exist_ok=True)
        shutil.copytree(src_inc, os.path.join(RES, "include"))
    dst_lib = os.path.join(RES, "lib", TARGET)
    os.makedirs(dst_lib, exist_ok=True)
    dst = os.path.join(dst_lib, "libclang_rt.builtins.a")
    cands = glob.glob(os.path.join(SYSROOT, "lib", "clang", "*", "lib", "windows", "libclang_rt.builtins-aarch64.a"))
    if not cands:
        die("sysroot 里没有 libclang_rt.builtins-aarch64.a（缺 compiler-rt 包）")
    if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(cands[0]):
        shutil.copyfile(cands[0], dst)
    return dst


def compile_one(src, extra=None):
    os.makedirs(OBJ, exist_ok=True)
    obj = os.path.join(OBJ, os.path.basename(src).replace(".c", ".o"))
    cmd = [CLANG] + BASE + (extra or []) + ["-c", src, "-o", obj]
    run(cmd, "编译 " + os.path.basename(src))
    return obj


def link(out, objs, extra):
    cmd = [CLANG, "--target=" + TARGET, "--sysroot=" + SYSROOT, "-resource-dir=" + RES,
           "-rtlib=compiler-rt", "-fuse-ld=lld", "-o", out] + objs + extra
    run(cmd, "链接 " + os.path.basename(out))


def pe_machine(path):
    with open(path, "rb") as f:
        head = f.read(2048)
    off = struct.unpack_from("<I", head, 0x3C)[0]
    return struct.unpack_from("<H", head, off + 4)[0]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    t0 = time.time()
    builtins = prepare_resource_dir()
    print("builtins:", builtins)
    if os.path.isdir(OBJ):
        shutil.rmtree(OBJ, ignore_errors=True)
    os.makedirs(OBJ, exist_ok=True)

    objs = []
    for src in sorted(glob.glob(os.path.join(ROOT, "src", "core", "*.c"))):
        objs.append(compile_one(src))
    for src in sorted(glob.glob(os.path.join(ROOT, "src", "platform", "win", "*.c"))):
        objs.append(compile_one(src))
    objs.append(compile_one(os.path.join(ROOT, "third_party", "sqlite", "sqlite3.c"),
                            ["-w", "-DSQLITE_THREADSAFE=1", "-DSQLITE_OMIT_LOAD_EXTENSION",
                             "-DSQLITE_DQS=0", "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1"]))
    objs.append(compile_one(os.path.join(ROOT, "src", "ui", "win", "dsc_app.c"), ["-DUNICODE", "-D_UNICODE"]))

    exe = os.path.join(BUILD, "DSC.exe")
    link(exe, objs, ["-mwindows"] + LIBS)

    host_obj = compile_one(os.path.join(ROOT, "src", "plugin_runner", "dsc_plugin_host.c"),
                           ["-I" + os.path.join(ROOT, "src", "plugin_runner")])
    host = os.path.join(BUILD, "dsc_plugin_host.exe")
    link(host, [host_obj], ["-lkernel32", "-luser32"])

    dll = os.path.join(BUILD, "sample_basic.dll")
    link(dll, [os.path.join(ROOT, "plugins", "sample_basic", "sample_basic.c"),
               "-I" + os.path.join(ROOT, "src", "plugin_runner")],
         ["-shared", "-lkernel32"])

    print("")
    for path in (exe, host, dll):
        machine = pe_machine(path)
        note = "ARM64" if machine == 0xAA64 else ("x64" if machine == 0x8664 else "0x%04x" % machine)
        print("%-22s %9d 字节  %-6s  sha256 %s" % (os.path.basename(path), os.path.getsize(path),
                                                  note, sha256_of(path)))
    print("")
    print("耗时 %.1fs" % (time.time() - t0))
    print("提示：本机是 x64，无法运行 ARM64 产物，只能在 ARM64 真机（或仿真环境）验证启动与交互。")


if __name__ == "__main__":
    main()
