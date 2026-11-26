#!/usr/bin/env python3
# gen_persona.py —— 把 resources/persona/dsc_default_persona.txt 生成内嵌的默认人设 C 字符串。
#
# 为什么用「文本源 + 生成」而不是直接把长文本写成 C 宏：
#   - 人设是四五十字一段的中文长文，C 宏要手工拼接引号与换行，改一个字都可能漏转义；
#   - 文本文件可以直接对比 diff，也能被 verify/precommit 静态检查读到原文；
#   - 生成是确定性的：同样的输入逐字节相同，--check 能确认生成物没过期。
#
# 用法：
#   python tools/gen_persona.py          # 重新生成 src/core/persona_default.c
#   python tools/gen_persona.py --check  # 只比对，不一致时退出码 1（不写文件）
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "resources", "persona", "dsc_default_persona.txt")
OUT_C = os.path.join(ROOT, "src", "core", "persona_default.c")

# 提示词里必须保留的变量占位（与 src/core/prompt.c 的替换逻辑一致）。
PLACEHOLDERS = ["{user_name}", "{user_gender}", "{user_birthday}", "{user_occupation}", "{user_extra_note}"]

# 人设正文太短基本等于被清空了，直接失败。
MIN_CHARS = 800


def fail(msg):
    print("[FAIL] " + msg, file=sys.stderr)
    sys.exit(1)


def c_string_literal(text):
    """逐行转义成若干 C 字符串字面量（每行一个，便于 diff 与阅读）。"""
    out = []
    for line in text.split("\n"):
        esc = []
        for ch in line:
            o = ord(ch)
            if ch == "\\":
                esc.append("\\\\")
            elif ch == '"':
                esc.append('\\"')
            elif ch == "\t":
                esc.append("\\t")
            elif o < 0x20 or o == 0x7F:
                esc.append("\\x%02x" % o)
            else:
                esc.append(ch)
        out.append('    "' + "".join(esc) + '\\n"')
    return "\n".join(out)


def build(text):
    body = c_string_literal(text.rstrip("\n"))
    return (
        "/* persona_default.c —— 由 tools/gen_persona.py 从 resources/persona/dsc_default_persona.txt 生成，请勿手改。\n"
        " *\n"
        " * 这是「全新安装」时种进 persona 表的内置默认人设：只有库里一条人设都没有时才会写入，\n"
        " * 已有用户的旧人设不会被覆盖（改人设不迁移，理由见 docs/DESIGN.md）。\n"
        " *\n"
        " * 重新生成：python tools/gen_persona.py\n"
        " */\n"
        '#include "persona_default.h"\n\n'
        "const char DSC_PERSONA_DEFAULT_PROMPT[] =\n"
        + body
        + ";\n"
    )


def main():
    check = "--check" in sys.argv[1:]
    if not os.path.exists(SRC):
        fail("找不到人设源文件：%s" % SRC)
    with open(SRC, "rb") as f:
        raw = f.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        fail("人设源文件不是合法 UTF-8：%s" % e)
    if "\r" in text:
        fail("人设源文件不能含 CR（统一用 LF，避免生成物随编辑器变化）")
    if len(text.strip()) < MIN_CHARS:
        fail("人设正文太短（%d 字符，至少 %d）" % (len(text.strip()), MIN_CHARS))
    missing = [p for p in PLACEHOLDERS if p not in text]
    if missing:
        fail("人设正文缺少变量占位：%s" % ", ".join(missing))

    c = build(text)
    if check:
        if not os.path.exists(OUT_C):
            fail("生成物不存在：%s" % OUT_C)
        with open(OUT_C, "r", encoding="utf-8") as f:
            if f.read() != c:
                fail("生成物已过期，请重跑 python tools/gen_persona.py")
        print("ok: src/core/persona_default.c 与源文本一致（%d 字）" % len(text.strip()))
        return

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)
    print("built: src/core/persona_default.c（%d 字，%d 字节 C 源码）" % (len(text.strip()), len(c.encode("utf-8"))))


if __name__ == "__main__":
    main()
