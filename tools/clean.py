# -*- coding: utf-8 -*-
"""clean.py —— 一键清理构建、测试与打包留下的本地残留

用法：
    python tools/clean.py            清理构建目录、测试临时目录与调试残留文件
    python tools/clean.py --all      额外清空 dist/（发布产物也会被删，需重打包）
    python tools/clean.py --dry-run  只列出将删除的内容，不实际删除

清理范围（全部在 .gitignore 覆盖范围内，删了不影响 git status）：
    1. 仓库根的 dsc_test_* 目录（单元测试与传输测试在 cwd 下建的临时目录）
    2. build/、build_android/（构建脚本可完整重建）
    3. 仓库根散落的临时文件：*.log、*.err、*.tmp、core.<pid>、nul
    4. dist/ 中的旧版本产物（只保留当前版本号的包与 .sha256；--all 时整个 dist/）
    5. tools 目录下的 __pycache__/
"""

import argparse
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 与 build.ps1 / AndroidManifest.xml 保持一致：只按这个版本号保留 dist 产物
CURRENT_VERSION = "0.1.6"

BUILD_DIRS = ["build", "build_android"]
ROOT_JUNK_PATTERNS = [r"^dsc_test_", r"^core\.\d+$"]
ROOT_JUNK_NAMES = ["nul", "NUL"]
ROOT_JUNK_EXTS = [".log", ".err", ".tmp"]


def _rm(path, dry, removed):
    if not os.path.exists(path):
        return
    kind = "目录" if os.path.isdir(path) else "文件"
    removed.append("%s  %s  %s" % (kind, path, "（预演）" if dry else "已删除"))
    if dry:
        return
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path, ignore_errors=True)
    else:
        try:
            os.remove(path)
        except OSError:
            pass


def clean_root_junk(dry, removed):
    for name in sorted(os.listdir(ROOT)):
        full = os.path.join(ROOT, name)
        is_dir = os.path.isdir(full)
        hit = any(re.match(p, name) for p in ROOT_JUNK_PATTERNS)
        if not hit and name in ROOT_JUNK_NAMES:
            hit = True
        if not hit and not is_dir and os.path.splitext(name)[1].lower() in ROOT_JUNK_EXTS:
            hit = True
        if hit:
            _rm(full, dry, removed)


def clean_build_dirs(dry, removed):
    for d in BUILD_DIRS:
        _rm(os.path.join(ROOT, d), dry, removed)


def clean_dist(dry, removed, wipe_all):
    dist = os.path.join(ROOT, "dist")
    if not os.path.isdir(dist):
        return
    if wipe_all:
        _rm(dist, dry, removed)
        return
    keep_prefix = "DSC-%s-" % CURRENT_VERSION
    for name in sorted(os.listdir(dist)):
        full = os.path.join(dist, name)
        if os.path.isdir(full) or not name.startswith(keep_prefix):
            _rm(full, dry, removed)


def clean_pycache(dry, removed):
    tools = os.path.join(ROOT, "tools")
    for base, dirs, _files in os.walk(tools):
        for d in list(dirs):
            if d == "__pycache__":
                _rm(os.path.join(base, d), dry, removed)
                dirs.remove(d)


def main():
    ap = argparse.ArgumentParser(description="清理 DSC 本地构建与测试残留")
    ap.add_argument("--all", action="store_true", help="连 dist/ 发布产物一起清空")
    ap.add_argument("--dry-run", action="store_true", help="只列出将删除的内容")
    args = ap.parse_args()

    removed = []
    clean_root_junk(args.dry_run, removed)
    clean_build_dirs(args.dry_run, removed)
    clean_dist(args.dry_run, removed, args.all)
    clean_pycache(args.dry_run, removed)

    if not removed:
        print("clean: 没有需要清理的内容")
        return 0
    for line in removed:
        print(line)
    print("clean: 共 %d 项%s" % (len(removed), "（预演）" if args.dry_run else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
