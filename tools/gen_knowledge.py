#!/usr/bin/env python3
# gen_knowledge.py —— 把 resources/knowledge/knowledge.json 生成内嵌 C 数据。
#
# 内置知识库随包离线分发（技术设计 §17 / FR-31、FR-32）：
#   - 生成一个 C 字符串常量，编译进二进制，不依赖外部文件、不依赖外链；
#   - 生成前做完整性校验，字段缺失或来源记录不合法直接失败，避免半成品进包；
#   - 输出是确定性的：同样的输入逐字节相同，便于比对与复现。
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "resources", "knowledge", "knowledge.json")
OUT_C = os.path.join(ROOT, "src", "core", "knowledge_data.c")
CHUNK = 2000

REQUIRED_GROUPS = ["memory", "prompt", "context", "longchat", "token"]


def fail(msg):
    print("[FAIL] " + msg, file=sys.stderr)
    sys.exit(1)


def need(obj, key, where):
    if key not in obj:
        fail("%s 缺少字段 %s" % (where, key))
    return obj[key]


def validate(kb):
    for k in ("version", "updated_at", "changelog", "groups"):
        need(kb, k, "根对象")
    if not kb["changelog"]:
        fail("变更日志不能为空")
    for ch in kb["changelog"]:
        for k in ("version", "date", "note"):
            need(ch, k, "变更日志条目")
    groups = kb["groups"]
    if len(groups) != 5:
        fail("知识库必须是五组，实际 %d 组" % len(groups))
    ids = [g.get("id") for g in groups]
    for gid in REQUIRED_GROUPS:
        if gid not in ids:
            fail("缺少知识组 %s" % gid)
    n_articles = 0
    for g in groups:
        for k in ("id", "title", "summary", "articles"):
            need(g, k, "知识组 " + str(g.get("id")))
        if len(g["articles"]) < 3:
            fail("知识组 %s 至少三篇，实际 %d 篇" % (g["id"], len(g["articles"])))
        seen = set()
        for a in g["articles"]:
            where = "文章 " + str(a.get("id"))
            for k in ("id", "title", "summary", "body", "diagram", "example", "points", "sources"):
                need(a, k, where)
            if a["id"] in seen:
                fail("文章 id 重复：%s" % a["id"])
            seen.add(a["id"])
            if len(a["body"]) < 2:
                fail("%s 正文至少两段" % where)
            if len(a["diagram"]) < 2:
                fail("%s 示意图节点至少两个" % where)
            if len(a["points"]) < 1:
                fail("%s 要点卡片不能为空" % where)
            if not a["sources"]:
                fail("%s 必须记录来源" % where)
            for s in a["sources"]:
                for k in ("title", "author", "url", "license", "date"):
                    need(s, k, where + " 的来源")
                if not s["url"].startswith("http"):
                    fail("%s 的来源链接必须是 http(s)" % where)
                if not s["license"]:
                    fail("%s 的来源必须写许可证" % where)
            n_articles += 1
    return n_articles


def c_string_literal(text):
    out = []
    for i in range(0, len(text), CHUNK):
        part = text[i:i + CHUNK]
        esc = []
        for ch in part:
            o = ord(ch)
            if ch == "\\":
                esc.append("\\\\")
            elif ch == '"':
                esc.append('\\"')
            elif ch == "\n":
                esc.append("\\n")
            elif ch == "\t":
                esc.append("\\t")
            elif ch == "\r":
                esc.append("\\r")
            elif o < 0x20 or o == 0x7F:
                esc.append("\\x%02x" % o)
            else:
                esc.append(ch)
        out.append('    "' + "".join(esc) + '"')
    return "\n".join(out)


def main():
    if not os.path.exists(SRC):
        fail("找不到知识库源文件：%s" % SRC)
    with open(SRC, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    try:
        kb = json.loads(text)
    except json.JSONDecodeError as e:
        fail("知识库 JSON 不合法：%s" % e)
    n = validate(kb)
    compact = json.dumps(kb, ensure_ascii=False, separators=(",", ":"), sort_keys=False)
    body = c_string_literal(compact)
    c = (
        "/* knowledge_data.c —— 由 tools/gen_knowledge.py 从 resources/knowledge/knowledge.json 生成，请勿手改。\n"
        " *\n"
        " * 为什么内嵌进二进制而不是运行时读文件：内置知识库要随包离线分发（§17），\n"
        " * 客户端可能被拷贝到任意目录、手机端还要从 assets 读取，内嵌让两端取同一份数据，\n"
        " * 也避免「资源文件被删导致知识库空白」这类现场问题。\n"
        " *\n"
        " * 重新生成：python tools/gen_knowledge.py\n"
        " */\n"
        "#include \"knowledge.h\"\n\n"
        "const char *aic_knowledge_json(void)\n{\n    static const char kb[] =\n"
        + body
        + ";\n    return kb;\n}\n\n"
        "const size_t aic_knowledge_json_len(void)\n{\n    static const size_t n = sizeof(kb_len_probe);\n    (void)n;\n    return 0;\n}\n"
    )
    # 长度直接由调用方 strlen 取得，这里不额外生成探测变量。
    c = c.replace(
        "const size_t aic_knowledge_json_len(void)\n{\n    static const size_t n = sizeof(kb_len_probe);\n    (void)n;\n    return 0;\n}\n", "")
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)
    print("built: src/core/knowledge_data.c（%d 篇，%d 字节 JSON，五组）" % (n, len(compact.encode("utf-8"))))


if __name__ == "__main__":
    main()
