#!/usr/bin/env python3
"""提交前扫描（敏感字符串 + 许可证 + 大文件 + 源码头 + 二进制混入）。

用法：
    python tools/precommit_scan.py [仓库根目录] [--quiet]

退出码 = 问题数量（0 表示干净）。CI 与 CONTRIBUTING 都要求提交前跑一次。

检查项：
    1. 敏感字符串：API Key 形态、私钥块、口令赋值
    2. 许可证：LICENSE 必须是 Apache-2.0；third_party 必须有许可说明；源码不得出现 GPL/AGPL 声明
    3. 源码头：每个 .c/.h/.java 第一行必须是注释且写明自身文件名
    4. 大文件：third_party 之外不得有 >1 MB 的文本文件（防二进制误提交）
    5. 二进制混入：.c/.h 里不得出现 NUL 字节
    6. 产物目录：dist / build / build_android 等不应被跟踪（提示性，不判失败）
"""

import os
import re
import sys

# CI（GitHub Actions 的 Windows runner）默认把 stdout 当 cp1252，中文 print 会直接崩，
# 所以先把标准输出改成 UTF-8，本地与 CI 行为一致。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

SKIP_DIRS = {
    ".git", "build", "build_android", "dist", "obj", "shotsdata", "verify", "bench",
    ".vscode", ".idea", "__pycache__", "node_modules", "tmp",
}
TEXT_EXT = {
    ".c", ".h", ".md", ".txt", ".py", ".mjs", ".ps1", ".java", ".xml", ".yml", ".yaml",
    ".json", ".gitignore", ".gradle", ".properties", "",
}
SOURCE_EXT = {".c", ".h", ".java"}

# 敏感字符串规则：(名称, 正则, 允许出现的文件后缀例外)
SENSITIVE = [
    ("OpenAI/DeepSeek 风格 Key", re.compile(r"\bsk-[A-Za-z0-9_\-]{16,}"), None),
    ("GitHub Token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}"), None),
    ("AWS Access Key", re.compile(r"\bAKIA[0-9A-Z]{16}\b"), None),
    ("私钥块", re.compile(r"BEGIN (?:RSA |EC |OPENSSH |PGP )?PRIVATE KEY"), None),
    ("疑似硬编码口令", re.compile(r"""(?i)\b(?:passwd|password|secret|api_?key)\s*[:=]\s*['"][^'"\s]{8,}['"]"""), None),
]
# 按文件豁免（都已核实为故意包含敏感形态的说明或假夹具，不含真实凭证）
EXEMPT = {
    "src/core/util.c": "敏感字符串扫描器的模式表本身",
    "tools/precommit_scan.py": "扫描器自身",
    "tests/unit/test_core.c": "测试夹具，全部为假凭证占位",
    "src/cli/aic_verify.c": "验收器夹具，全部为假凭证占位",
}

# 明确允许的占位口令（本地自签名 keystore，仅开发期使用，不涉及任何线上凭证）
ALLOW_LINE = re.compile(r"sai123|SAI_DEV_KEY_PLACEHOLDER")

BAD_LICENSE = re.compile(r"(?i)(GNU (?:Lesser |Affero )?General Public License|AGPL|GPLv[23])")

problems = []      # (类别, 文件, 行号, 描述)
notes = []         # 提示，不判失败


def rel(root, path):
    return os.path.relpath(path, root).replace("\\", "/")


def iter_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            yield os.path.join(dirpath, name)


def read_text(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None, None
    return data, data.decode("utf-8", errors="replace")


def check_sensitive(root, path, text):
    if rel(root, path) in EXEMPT:
        return
    for lineno, line in enumerate(text.splitlines(), 1):
        if ALLOW_LINE.search(line):
            continue
        for label, rx, _exc in SENSITIVE:
            if rx.search(line):
                problems.append(("敏感字符串", rel(root, path), lineno, label))
                break


def check_license(root, path, text):
    r = rel(root, path)
    if r == "tools/precommit_scan.py":
        return  # 本文件的正则与文档里必然出现这些许可名称
    if r == "LICENSE":
        if "Apache License" not in text or "Version 2.0, January 2004" not in text:
            problems.append(("许可证", r, 0, "LICENSE 不是 Apache-2.0 文本"))
    if r.startswith("third_party/") and path.endswith("README.md"):
        if "Public Domain" not in text and "License" not in text:
            problems.append(("许可证", r, 0, "第三方目录缺少许可说明"))
    # 只在源码与清单里查"许可声明"，文档里出现 GPL/AGPL 多是在说明规则本身，不算问题。
    if not r.startswith("third_party/") and os.path.splitext(path)[1] in (".c", ".h", ".java", ".json", ".xml"):
        for lineno, line in enumerate(text.splitlines(), 1):
            m = BAD_LICENSE.search(line)
            if m:
                problems.append(("许可证", r, lineno, "出现不兼容的许可声明：" + m.group(1)))


def check_header(root, path, text):
    if os.path.splitext(path)[1] not in SOURCE_EXT:
        return
    r = rel(root, path)
    if not (r.startswith("src/") or r.startswith("tests/") or r.startswith("plugins/")
            or r.startswith("android/")):
        return
    lines = text.splitlines()[:5]
    first = lines[0] if lines else ""
    base = os.path.basename(path)
    stripped = first.strip()
    if not (stripped.startswith("/*") or stripped.startswith("//")):
        problems.append(("源码头", r, 1, "第一行不是注释"))
    elif not any(base in ln for ln in lines):
        problems.append(("源码头", r, 1, "头注释前 5 行没有写明文件名 " + base))


def check_size(root, path, data):
    r = rel(root, path)
    if r.startswith("third_party/"):
        return
    if os.path.splitext(path)[1] not in TEXT_EXT:
        return
    if len(data) > 1024 * 1024:
        problems.append(("大文件", r, 0, "%.1f MB，超过 1 MB 上限" % (len(data) / 1048576.0)))


def check_binary(root, path, data, text):
    if os.path.splitext(path)[1] not in SOURCE_EXT:
        return
    if b"\x00" in data:
        problems.append(("二进制混入", rel(root, path), 0, "源码里出现 NUL 字节"))


def main(argv):
    root = os.path.abspath(argv[1]) if len(argv) > 1 and not argv[1].startswith("-") else os.getcwd()
    quiet = "--quiet" in argv
    if not os.path.isdir(root):
        print("目录不存在：%s" % root)
        return 2

    seen_tracked = 0
    for path in iter_files(root):
        data, text = read_text(path)
        if data is None:
            continue
        seen_tracked += 1
        if os.path.splitext(path)[1] in TEXT_EXT or os.path.basename(path) in (".gitignore", "LICENSE"):
            if text is not None:
                check_sensitive(root, path, text)
                check_license(root, path, text)
                check_header(root, path, text)
                check_binary(root, path, data, text)
        check_size(root, path, data)

    if not os.path.exists(os.path.join(root, "LICENSE")):
        problems.append(("许可证", "LICENSE", 0, "仓库根目录缺少 LICENSE"))

    for d in ("dist", "build", "build_android"):
        p = os.path.join(root, d)
        if os.path.isdir(p):
            notes.append("产物目录 %s 存在（发布时用，提交前不必删除，但不应被 git 跟踪）" % d)

    if not quiet:
        print("Sai 提交前扫描")
        print("  扫描目录：%s" % root)
        print("  文件数：%d" % seen_tracked)
        for n in notes:
            print("  提示：" + n)
        for f, why in sorted(EXEMPT.items()):
            print("  豁免：%s（%s）" % (f, why))
        if problems:
            print("\n发现 %d 个问题：" % len(problems))
            for cat, f, line, desc in problems:
                loc = "%s:%d" % (f, line) if line else f
                print("  [%s] %s —— %s" % (cat, loc, desc))
        else:
            print("\n未发现问题：敏感字符串 0、许可证合规、源码头齐备、无超大文件、无二进制混入。")
    return len(problems)


if __name__ == "__main__":
    sys.exit(main(sys.argv[:]))
