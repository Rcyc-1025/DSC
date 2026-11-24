/* knowledge.c —— 内置知识库的解析、检索、书签与引用片段（§17 / FR-31、FR-32）。 */
#include "knowledge.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "store_setting.h"

/* ---------- 内嵌数据 ---------- */

/* 数据本体在 knowledge_data.c（生成文件）里，这里只声明用得到的那一个符号。 */
extern const char *dsc_knowledge_json(void);

/* ---------- 小工具 ---------- */

/* 把 JSON 数组里的字符串用 '\n' 拼进 out（正文、示意图节点、要点卡片都用这个格式）。 */
static void join_string_array(const jv *arr, sb *out)
{
    size_t n = jv_len(arr);
    for (size_t i = 0; i < n; i++) {
        const char *s = jv_string(jv_at(arr, i));
        if (!s) continue;
        if (i > 0) sb_appendc(out, '\n');
        sb_append(out, s);
    }
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (src) dsc_strlcpy_utf8(dst, cap, src);
}

/* 解析一篇；调用方已保证 a 非空。 */
static int parse_article(const jv *v, const char *group_id, dsc_kb_article *a)
{
    if (!v || v->type != JV_OBJ) return DSC_E_FORMAT;
    memset(a, 0, sizeof(*a));
    copy_str(a->id, sizeof(a->id), jv_get_str(v, "id", ""));
    copy_str(a->group_id, sizeof(a->group_id), group_id);
    copy_str(a->title, sizeof(a->title), jv_get_str(v, "title", ""));
    copy_str(a->summary, sizeof(a->summary), jv_get_str(v, "summary", ""));
    if (!a->id[0] || !a->title[0]) return DSC_E_FORMAT;

    sb tmp;
    sb_init(&tmp);
    join_string_array(jv_get(v, "body"), &tmp);
    copy_str(a->body, sizeof(a->body), sb_cstr(&tmp));
    sb_reset(&tmp);
    join_string_array(jv_get(v, "diagram"), &tmp);
    copy_str(a->diagram, sizeof(a->diagram), sb_cstr(&tmp));
    sb_reset(&tmp);
    join_string_array(jv_get(v, "points"), &tmp);
    copy_str(a->points, sizeof(a->points), sb_cstr(&tmp));
    sb_free(&tmp);
    copy_str(a->example, sizeof(a->example), jv_get_str(v, "example", ""));

    const jv *srcs = jv_get(v, "sources");
    size_t sn = jv_len(srcs);
    for (size_t i = 0; i < sn && a->source_n < DSC_KB_MAX_SOURCES; i++) {
        const jv *s = jv_at(srcs, i);
        dsc_kb_source *d = &a->sources[a->source_n];
        copy_str(d->title, sizeof(d->title), jv_get_str(s, "title", ""));
        copy_str(d->author, sizeof(d->author), jv_get_str(s, "author", ""));
        copy_str(d->url, sizeof(d->url), jv_get_str(s, "url", ""));
        copy_str(d->license, sizeof(d->license), jv_get_str(s, "license", ""));
        copy_str(d->date, sizeof(d->date), jv_get_str(s, "date", ""));
        if (!d->title[0] || !d->url[0] || !d->license[0]) return DSC_E_FORMAT;
        a->source_n++;
    }
    if (a->source_n == 0) return DSC_E_FORMAT;   /* 每条记录必须有来源（§17） */
    return DSC_OK;
}

int dsc_kb_load(dsc_kb **out, char *err, size_t errlen)
{
    if (!out) return DSC_E_INVAL;
    *out = NULL;
    if (err && errlen) err[0] = '\0';

    const char *text = dsc_knowledge_json();
    if (!text || !text[0]) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "知识库数据为空");
        return DSC_E_FORMAT;
    }
    jv *root = NULL;
    int rc = jv_parse(text, strlen(text), &root, err, errlen);
    if (rc != DSC_OK) return rc;

    dsc_kb *kb = (dsc_kb *)dsc_xcalloc(1, sizeof(dsc_kb));
    if (!kb) { jv_free(root); return DSC_E_IO; }

    copy_str(kb->version, sizeof(kb->version), jv_get_str(root, "version", ""));
    copy_str(kb->updated_at, sizeof(kb->updated_at), jv_get_str(root, "updated_at", ""));

    /* 变更日志：每条一行 "version|date|note"。 */
    sb cl;
    sb_init(&cl);
    const jv *chg = jv_get(root, "changelog");
    size_t cn = jv_len(chg);
    for (size_t i = 0; i < cn; i++) {
        const jv *e = jv_at(chg, i);
        if (i > 0) sb_appendc(&cl, '\n');
        sb_appendf(&cl, "%s|%s|%s", jv_get_str(e, "version", "-"),
                   jv_get_str(e, "date", "-"), jv_get_str(e, "note", ""));
    }
    copy_str(kb->changelog, sizeof(kb->changelog), sb_cstr(&cl));
    sb_free(&cl);

    const jv *groups = jv_get(root, "groups");
    size_t gn = jv_len(groups);
    int bad = (!kb->version[0] || !kb->changelog[0] || gn < 5 || gn > (size_t)DSC_KB_MAX_GROUPS);
    int ai = 0;
    for (size_t gi = 0; gi < gn && !bad && gi < (size_t)DSC_KB_MAX_GROUPS; gi++) {
        const jv *g = jv_at(groups, gi);
        dsc_kb_group *gout = &kb->groups[kb->group_n];
        copy_str(gout->id, sizeof(gout->id), jv_get_str(g, "id", ""));
        copy_str(gout->title, sizeof(gout->title), jv_get_str(g, "title", ""));
        copy_str(gout->summary, sizeof(gout->summary), jv_get_str(g, "summary", ""));
        if (!gout->id[0] || !gout->title[0]) { bad = 1; break; }
        gout->first = ai;
        const jv *arts = jv_get(g, "articles");
        size_t an = jv_len(arts);
        if (an == 0) { bad = 1; break; }
        for (size_t aj = 0; aj < an; aj++) {
            if (ai >= DSC_KB_MAX_ARTICLES) { bad = 1; break; }
            if (parse_article(jv_at(arts, aj), gout->id, &kb->articles[ai]) != DSC_OK) { bad = 1; break; }
            ai++;
        }
        gout->count = ai - gout->first;
        if (!bad) kb->group_n++;
    }
    kb->article_n = ai;
    jv_free(root);

    if (bad || kb->group_n < 5 || kb->article_n < 15) {
        dsc_kb_free(kb);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "知识库结构不完整（需五组、每组至少三篇、每篇含来源）");
        return DSC_E_FORMAT;
    }
    *out = kb;
    return DSC_OK;
}

void dsc_kb_free(dsc_kb *kb) { dsc_xfree(kb); }

const char *dsc_kb_version(const dsc_kb *kb) { return kb ? kb->version : ""; }
const char *dsc_kb_updated_at(const dsc_kb *kb) { return kb ? kb->updated_at : ""; }
const char *dsc_kb_changelog(const dsc_kb *kb) { return kb ? kb->changelog : ""; }

int dsc_kb_group_count(const dsc_kb *kb) { return kb ? kb->group_n : 0; }

const dsc_kb_group *dsc_kb_group_at(const dsc_kb *kb, int index)
{
    if (!kb || index < 0 || index >= kb->group_n) return NULL;
    return &kb->groups[index];
}

int dsc_kb_group_index_of(const dsc_kb *kb, const char *group_id)
{
    if (!kb || !group_id) return -1;
    for (int i = 0; i < kb->group_n; i++) {
        if (strcmp(kb->groups[i].id, group_id) == 0) return i;
    }
    return -1;
}

int dsc_kb_article_count(const dsc_kb *kb, int group_index)
{
    const dsc_kb_group *g = dsc_kb_group_at(kb, group_index);
    return g ? g->count : 0;
}

const dsc_kb_article *dsc_kb_article_at(const dsc_kb *kb, int group_index, int i)
{
    const dsc_kb_group *g = dsc_kb_group_at(kb, group_index);
    if (!g || i < 0 || i >= g->count) return NULL;
    return &kb->articles[g->first + i];
}

const dsc_kb_article *dsc_kb_find(const dsc_kb *kb, const char *article_id)
{
    if (!kb || !article_id) return NULL;
    for (int i = 0; i < kb->article_n; i++) {
        if (strcmp(kb->articles[i].id, article_id) == 0) return &kb->articles[i];
    }
    return NULL;
}

/* ---------- 检索 ---------- */

#define KB_MAX_TERMS 16
#define KB_TERM_CAP  32

static int is_cjk_cp(unsigned int cp)
{
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

/* 解码一个 UTF-8 码点，返回消耗字节数（非法字节按单字节处理）。 */
static size_t utf8_next(const char *s, unsigned int *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((unsigned int)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((unsigned int)(p[0] & 0x0F) << 12) | ((unsigned int)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((unsigned int)(p[0] & 0x07) << 18) | ((unsigned int)(p[1] & 0x3F) << 12) |
              ((unsigned int)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static int term_seen(char terms[][KB_TERM_CAP], int n, const char *t)
{
    for (int i = 0; i < n; i++) if (strcmp(terms[i], t) == 0) return 1;
    return 0;
}

/* 把查询切成词：中文取相邻两字（单字查询保留单字），英文数字取连续段并小写。 */
static int kb_split_terms(const char *q, char terms[][KB_TERM_CAP], int cap)
{
    if (!q || !q[0]) return 0;
    int n = 0;
    const char *p = q;
    unsigned int prev = 0;
    char prev_buf[8] = {0};
    size_t prev_len = 0;
    while (*p && n < cap) {
        unsigned int cp = 0;
        size_t adv = utf8_next(p, &cp);
        if (is_cjk_cp(cp)) {
            char cur[8] = {0};
            size_t cl = adv < sizeof(cur) - 1 ? adv : sizeof(cur) - 1;
            memcpy(cur, p, cl);
            if (prev_len && is_cjk_cp(prev)) {
                char term[KB_TERM_CAP];
                snprintf(term, sizeof(term), "%.*s%.*s", (int)prev_len, prev_buf, (int)cl, cur);
                if (!term_seen(terms, n, term)) snprintf(terms[n++], KB_TERM_CAP, "%s", term);
            }
            memcpy(prev_buf, cur, cl);
            prev_buf[cl] = '\0';
            prev_len = cl;
            prev = cp;
        } else if (cp < 0x80 && isalnum((int)cp)) {
            char word[KB_TERM_CAP];
            size_t w = 0;
            const char *r = p;
            while (*r && w < sizeof(word) - 1) {
                unsigned int c2 = 0;
                size_t a2 = utf8_next(r, &c2);
                if (c2 > 0x7F || !isalnum((unsigned char)c2)) break;
                word[w++] = (char)tolower((unsigned char)c2);
                r += a2;
            }
            word[w] = '\0';
            if (w >= 2 && !term_seen(terms, n, word)) snprintf(terms[n++], KB_TERM_CAP, "%s", word);
            p = r;
            prev_len = 0;
            prev = 0;
            continue;
        } else {
            prev_len = 0;
            prev = 0;
        }
        p += adv;
    }
    /* 全是中文单字（比如查「token」以外的单个汉字）时，补一个单字词，避免空查询。 */
    if (n == 0 && q[0]) {
        dsc_strlcpy_utf8(terms[n], KB_TERM_CAP, q);
        n = 1;
    }
    return n;
}

/* 统计 needle 在 haystack（已按 ASCII 小写化）里出现次数。 */
static int count_hits(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return 0;
    int c = 0;
    const char *p = haystack;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += nl;
    }
    return c;
}

/* 把 haystack 的 ASCII 大写转小写，写进 buf；返回 buf。 */
static char *ascii_lower_copy(const char *src, char *buf, size_t cap)
{
    size_t i = 0;
    if (!src) src = "";
    for (; src[i] && i + 1 < cap; i++) buf[i] = (char)tolower((unsigned char)src[i]);
    buf[i] = '\0';
    return buf;
}

static int kb_score(const dsc_kb_article *a, char terms[][KB_TERM_CAP], int nt)
{
    char buf[DSC_KB_BODY_CAP];
    int score = 0;
    ascii_lower_copy(a->title, buf, sizeof(buf));
    for (int i = 0; i < nt; i++) score += count_hits(buf, terms[i]) * 5;
    ascii_lower_copy(a->summary, buf, sizeof(buf));
    for (int i = 0; i < nt; i++) score += count_hits(buf, terms[i]) * 3;
    ascii_lower_copy(a->points, buf, sizeof(buf));
    for (int i = 0; i < nt; i++) score += count_hits(buf, terms[i]) * 2;
    ascii_lower_copy(a->body, buf, sizeof(buf));
    for (int i = 0; i < nt; i++) score += count_hits(buf, terms[i]);
    return score;
}

int dsc_kb_search(const dsc_kb *kb, const char *query, char *out_ids, size_t id_stride,
                  int cap, int *out_hit)
{
    if (out_hit) *out_hit = 0;
    if (!kb || !out_ids || cap <= 0 || id_stride < 64) return 0;
    char terms[KB_MAX_TERMS][KB_TERM_CAP];
    int nt = kb_split_terms(query, terms, KB_MAX_TERMS);
    if (nt == 0) return 0;

    int   scores[DSC_KB_MAX_ARTICLES];
    int   order[DSC_KB_MAX_ARTICLES];
    int   hit = 0;
    for (int i = 0; i < kb->article_n; i++) {
        scores[i] = kb_score(&kb->articles[i], terms, nt);
        if (scores[i] > 0) order[hit++] = i;
    }
    /* 按分数降序（同分保持原文顺序）。数据量只有十几篇，插入排序足够。 */
    for (int i = 1; i < hit; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && scores[order[j]] < scores[v]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }
    int n = hit < cap ? hit : cap;
    for (int i = 0; i < n; i++) {
        char *dst = out_ids + (size_t)i * id_stride;
        snprintf(dst, id_stride, "%s", kb->articles[order[i]].id);
    }
    if (out_hit) *out_hit = hit;
    return n;
}

/* ---------- 段落与引用 ---------- */

int dsc_kb_paragraph_count(const dsc_kb_article *a)
{
    if (!a || !a->body[0]) return 0;
    int n = 1;
    for (const char *p = a->body; *p; p++) if (*p == '\n') n++;
    return n;
}

const char *dsc_kb_paragraph(const dsc_kb_article *a, int index, int *out_len)
{
    if (out_len) *out_len = 0;
    if (!a || index < 0) return NULL;
    const char *p = a->body;
    for (int i = 0; i < index; i++) {
        const char *nl = strchr(p, '\n');
        if (!nl) return NULL;
        p = nl + 1;
    }
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    if (out_len) *out_len = len;
    return p;
}

const char *dsc_kb_source_line(const dsc_kb_source *s, char *buf, size_t cap)
{
    if (!buf || cap == 0) return "";
    buf[0] = '\0';
    if (!s) return buf;
    snprintf(buf, cap, "《%s》— %s（%s，%s）%s", s->title, s->author, s->license, s->date, s->url);
    return buf;
}

int dsc_kb_cite(const dsc_kb *kb, const char *article_id, int paragraph_index, sb *out)
{
    if (!kb || !article_id || !out) return DSC_E_INVAL;
    const dsc_kb_article *a = dsc_kb_find(kb, article_id);
    if (!a) return DSC_E_NOTFOUND;
    int len = 0;
    const char *p = dsc_kb_paragraph(a, paragraph_index, &len);
    if (!p) return DSC_E_INVAL;
    sb_appendf(out, "【知识库引用】《%s》第 %d 段：\n", a->title, paragraph_index + 1);
    sb_appendn(out, p, (size_t)len);
    sb_append(out, "\n来源：");
    for (int i = 0; i < a->source_n; i++) {
        char line[512];
        dsc_kb_source_line(&a->sources[i], line, sizeof(line));
        if (i) sb_append(out, "；");
        sb_append(out, line);
    }
    sb_appendf(out, "\n（知识包 %s）", kb->version);
    return DSC_OK;
}

/* ---------- 书签与最近阅读（存 setting 的 JSON 数组） ---------- */

static int ids_load(dsc_db *db, const char *key, char *ids, int stride, int cap, int *out_n)
{
    if (out_n) *out_n = 0;
    char raw[4096];
    if (dsc_setting_get(db, key, raw, sizeof(raw)) != DSC_OK || !raw[0]) return DSC_OK;
    jv *v = NULL;
    if (jv_parse(raw, strlen(raw), &v, NULL, 0) != DSC_OK) return DSC_OK; /* 坏了就当空，不让设置文件毁掉功能 */
    int n = 0;
    size_t len = jv_len(v);
    for (size_t i = 0; i < len && n < cap; i++) {
        const char *s = jv_string(jv_at(v, i));
        if (!s || !s[0]) continue;
        snprintf(ids + (size_t)n * stride, (size_t)stride, "%s", s);
        n++;
    }
    jv_free(v);
    if (out_n) *out_n = n;
    return DSC_OK;
}

static int ids_save(dsc_db *db, const char *key, const char *ids, int stride, int n)
{
    sb b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (int i = 0; i < n; i++) {
        if (i) sb_appendc(&b, ',');
        jw_escape_into(&b, ids + (size_t)i * stride);
    }
    sb_appendc(&b, ']');
    int rc = dsc_setting_set(db, key, sb_cstr(&b));
    sb_free(&b);
    return rc;
}

int dsc_kb_bookmark_has(dsc_db *db, const char *article_id)
{
    if (!db || !article_id) return 0;
    char ids[DSC_KB_MAX_ARTICLES * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_BOOKMARK, ids, 64, DSC_KB_MAX_ARTICLES, &n);
    for (int i = 0; i < n; i++) if (strcmp(ids + (size_t)i * 64, article_id) == 0) return 1;
    return 0;
}

int dsc_kb_bookmark_toggle(dsc_db *db, const char *article_id, int *out_on)
{
    if (out_on) *out_on = 0;
    if (!db || !article_id || !article_id[0]) return DSC_E_INVAL;
    char ids[DSC_KB_MAX_ARTICLES * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_BOOKMARK, ids, 64, DSC_KB_MAX_ARTICLES, &n);
    int found = -1;
    for (int i = 0; i < n; i++) if (strcmp(ids + (size_t)i * 64, article_id) == 0) { found = i; break; }
    if (found >= 0) {
        for (int i = found; i + 1 < n; i++) memmove(ids + (size_t)i * 64, ids + (size_t)(i + 1) * 64, 64);
        n--;
        if (out_on) *out_on = 0;
    } else {
        if (n >= DSC_KB_MAX_ARTICLES) return DSC_E_LIMIT;
        snprintf(ids + (size_t)n * 64, 64, "%s", article_id);
        n++;
        if (out_on) *out_on = 1;
    }
    return ids_save(db, DSC_SET_KB_BOOKMARK, ids, 64, n);
}

int dsc_kb_bookmark_list(dsc_db *db, sb *out_json)
{
    if (!db || !out_json) return DSC_E_INVAL;
    char ids[DSC_KB_MAX_ARTICLES * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_BOOKMARK, ids, 64, DSC_KB_MAX_ARTICLES, &n);
    sb_appendc(out_json, '[');
    for (int i = 0; i < n; i++) {
        if (i) sb_appendc(out_json, ',');
        jw_escape_into(out_json, ids + (size_t)i * 64);
    }
    sb_appendc(out_json, ']');
    return DSC_OK;
}

int dsc_kb_bookmark_count(dsc_db *db)
{
    if (!db) return 0;
    char ids[DSC_KB_MAX_ARTICLES * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_BOOKMARK, ids, 64, DSC_KB_MAX_ARTICLES, &n);
    return n;
}

int dsc_kb_recent_push(dsc_db *db, const char *article_id)
{
    if (!db || !article_id || !article_id[0]) return DSC_E_INVAL;
    char ids[DSC_KB_RECENT_MAX * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_RECENT, ids, 64, DSC_KB_RECENT_MAX, &n);
    int exists = 0;
    for (int i = 0; i < n; i++) if (strcmp(ids + (size_t)i * 64, article_id) == 0) { exists = 1; break; }
    if (exists) {
        int j = 0;
        char out[DSC_KB_RECENT_MAX * 64];
        snprintf(out, 64, "%s", article_id);
        j = 1;
        for (int i = 0; i < n && j < DSC_KB_RECENT_MAX; i++) {
            if (strcmp(ids + (size_t)i * 64, article_id) == 0) continue;
            memcpy(out + (size_t)j * 64, ids + (size_t)i * 64, 64);
            j++;
        }
        return ids_save(db, DSC_SET_KB_RECENT, out, 64, j);
    }
    char out[DSC_KB_RECENT_MAX * 64];
    snprintf(out, 64, "%s", article_id);
    int j = 1;
    for (int i = 0; i < n && j < DSC_KB_RECENT_MAX; i++) {
        memcpy(out + (size_t)j * 64, ids + (size_t)i * 64, 64);
        j++;
    }
    return ids_save(db, DSC_SET_KB_RECENT, out, 64, j);
}

int dsc_kb_recent_list(dsc_db *db, sb *out_json)
{
    if (!db || !out_json) return DSC_E_INVAL;
    char ids[DSC_KB_RECENT_MAX * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_RECENT, ids, 64, DSC_KB_RECENT_MAX, &n);
    sb_appendc(out_json, '[');
    for (int i = 0; i < n; i++) {
        if (i) sb_appendc(out_json, ',');
        jw_escape_into(out_json, ids + (size_t)i * 64);
    }
    sb_appendc(out_json, ']');
    return DSC_OK;
}

int dsc_kb_recent_count(dsc_db *db)
{
    if (!db) return 0;
    char ids[DSC_KB_RECENT_MAX * 64];
    int n = 0;
    ids_load(db, DSC_SET_KB_RECENT, ids, 64, DSC_KB_RECENT_MAX, &n);
    return n;
}

int dsc_kb_state_clear(dsc_db *db)
{
    if (!db) return DSC_E_INVAL;
    dsc_setting_del(db, DSC_SET_KB_BOOKMARK);
    dsc_setting_del(db, DSC_SET_KB_RECENT);
    return DSC_OK;
}
