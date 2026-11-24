/* json.c —— JSON 读写实现。 */
#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static jv *jv_new(jv_type t)
{
    jv *v = (jv *)dsc_xcalloc(1, sizeof(jv));
    v->type = t;
    return v;
}

void jv_free(jv *v)
{
    if (!v) return;
    switch (v->type) {
    case JV_STR:
        dsc_xfree(v->u.str);
        break;
    case JV_ARR:
        for (size_t i = 0; i < v->u.arr.n; i++) jv_free(v->u.arr.items[i]);
        dsc_xfree(v->u.arr.items);
        break;
    case JV_OBJ:
        for (size_t i = 0; i < v->u.obj.n; i++) {
            dsc_xfree(v->u.obj.keys[i]);
            jv_free(v->u.obj.vals[i]);
        }
        dsc_xfree(v->u.obj.keys);
        dsc_xfree(v->u.obj.vals);
        break;
    default:
        break;
    }
    dsc_xfree(v);
}

const jv *jv_get(const jv *obj, const char *key)
{
    if (!obj || obj->type != JV_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->u.obj.n; i++)
        if (strcmp(obj->u.obj.keys[i], key) == 0) return obj->u.obj.vals[i];
    return NULL;
}

const char *jv_get_str(const jv *obj, const char *key, const char *def)
{
    const jv *v = jv_get(obj, key);
    if (!v || v->type != JV_STR) return def;
    return v->u.str;
}

long long jv_get_ll(const jv *obj, const char *key, long long def)
{
    const jv *v = jv_get(obj, key);
    if (!v) return def;
    if (v->type == JV_BOOL) return v->u.b ? 1 : 0; /* 布尔取整便于统一判断 */
    if (v->type != JV_NUM) return def;
    return (long long)v->u.num;
}

int jv_get_int(const jv *obj, const char *key, int def)
{
    return (int)jv_get_ll(obj, key, def);
}

int jv_get_bool(const jv *obj, const char *key, int def)
{
    const jv *v = jv_get(obj, key);
    if (!v || v->type != JV_BOOL) return def;
    return v->u.b;
}

size_t jv_len(const jv *v)
{
    if (!v) return 0;
    if (v->type == JV_ARR) return v->u.arr.n;
    if (v->type == JV_OBJ) return v->u.obj.n;
    if (v->type == JV_STR) return strlen(v->u.str);
    return 0;
}

const jv *jv_at(const jv *v, size_t i)
{
    if (!v || v->type != JV_ARR || i >= v->u.arr.n) return NULL;
    return v->u.arr.items[i];
}

const char *jv_string(const jv *v)
{
    return (v && v->type == JV_STR) ? v->u.str : NULL;
}

double jv_number(const jv *v)
{
    return (v && v->type == JV_NUM) ? v->u.num : 0.0;
}

int jv_bool(const jv *v)
{
    return (v && v->type == JV_BOOL) ? v->u.b : 0;
}

/* --------------- 写入 --------------- */
void jw_escape_into(sb *b, const char *s)
{
    sb_appendc(b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"': sb_append(b, "\\\""); break;
            case '\\': sb_append(b, "\\\\"); break;
            case '\b': sb_append(b, "\\b"); break;
            case '\f': sb_append(b, "\\f"); break;
            case '\n': sb_append(b, "\\n"); break;
            case '\r': sb_append(b, "\\r"); break;
            case '\t': sb_append(b, "\\t"); break;
            default:
                if (*p < 0x20) sb_appendf(b, "\\u%04x", *p);
                else sb_appendc(b, (char)*p);
            }
        }
    }
    sb_appendc(b, '"');
}

void jw_init(jw *w, sb *b, int pretty)
{
    memset(w, 0, sizeof(*w));
    w->b = b;
    w->pretty = pretty;
    w->depth = 0;
    w->first[0] = 1;
}

static void jw_indent(jw *w)
{
    if (!w->pretty) return;
    sb_appendc(w->b, '\n');
    for (int i = 0; i < w->depth; i++) sb_append(w->b, "  ");
}

static void jw_prefix(jw *w, const char *key)
{
    /* 当前层不是第一个元素时补逗号。 */
    if (!w->first[w->depth]) sb_appendc(w->b, ',');
    jw_indent(w);
    w->first[w->depth] = 0;
    if (key) {
        jw_escape_into(w->b, key);
        sb_appendc(w->b, ':');
        if (w->pretty) sb_appendc(w->b, ' ');
    }
}

void jw_obj_begin(jw *w, const char *key)
{
    jw_prefix(w, key);
    sb_appendc(w->b, '{');
    w->depth++;
    if (w->depth < 48) w->first[w->depth] = 1;
}

void jw_obj_end(jw *w)
{
    int was_empty = w->first[w->depth];
    w->depth--;
    if (!was_empty) jw_indent(w);
    sb_appendc(w->b, '}');
}

void jw_arr_begin(jw *w, const char *key)
{
    jw_prefix(w, key);
    sb_appendc(w->b, '[');
    w->depth++;
    if (w->depth < 48) w->first[w->depth] = 1;
}

void jw_arr_end(jw *w)
{
    int was_empty = w->first[w->depth];
    w->depth--;
    if (!was_empty) jw_indent(w);
    sb_appendc(w->b, ']');
}

void jw_put_str(jw *w, const char *key, const char *val)
{
    jw_prefix(w, key);
    if (val) jw_escape_into(w->b, val);
    else sb_append(w->b, "null");
}

void jw_put_int(jw *w, const char *key, long long val)
{
    jw_prefix(w, key);
    sb_appendf(w->b, "%lld", val);
}

void jw_put_bool(jw *w, const char *key, int val)
{
    jw_prefix(w, key);
    sb_append(w->b, val ? "true" : "false");
}

void jw_put_null(jw *w, const char *key)
{
    jw_prefix(w, key);
    sb_append(w->b, "null");
}

void jw_put_double(jw *w, const char *key, double val)
{
    jw_prefix(w, key);
    if (!isfinite(val)) {
        sb_append(w->b, "null");
        return;
    }
    if (val == (double)(long long)val) sb_appendf(w->b, "%lld", (long long)val);
    else sb_appendf(w->b, "%.6f", val);
}

void jw_put_raw(jw *w, const char *key, const char *raw)
{
    jw_prefix(w, key);
    sb_append(w->b, raw ? raw : "null");
}

/* --------------- 解析 --------------- */
typedef struct jparse {
    const char *s;
    size_t      len;
    size_t      pos;
    int         line;
    int         col;
    char       *err;
    size_t      errlen;
    int         failed;
} jparse;

static void jerr(jparse *p, const char *fmt, ...)
{
    if (p->failed) return;
    p->failed = 1;
    if (!p->err || p->errlen == 0) return;
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    snprintf(p->err, p->errlen, "第 %d 行第 %d 列：%s", p->line, p->col, body);
}

static void advance(jparse *p)
{
    if (p->pos < p->len) {
        if (p->s[p->pos] == '\n') { p->line++; p->col = 1; }
        else p->col++;
        p->pos++;
    }
}

static void skip_ws(jparse *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') advance(p);
        else break;
    }
}

static jv *parse_value(jparse *p, int depth);

static jv *parse_string(jparse *p)
{
    if (p->s[p->pos] != '"') { jerr(p, "期望字符串起始引号"); return NULL; }
    advance(p);
    sb b;
    sb_init(&b);
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == '"') { advance(p); 
            jv *v = jv_new(JV_STR);
            v->u.str = dsc_strdup(sb_cstr(&b));
            sb_free(&b);
            return v;
        }
        if (c == '\\') {
            advance(p);
            if (p->pos >= p->len) break;
            char e = p->s[p->pos];
            switch (e) {
            case '"': sb_appendc(&b, '"'); advance(p); break;
            case '\\': sb_appendc(&b, '\\'); advance(p); break;
            case '/': sb_appendc(&b, '/'); advance(p); break;
            case 'b': sb_appendc(&b, '\b'); advance(p); break;
            case 'f': sb_appendc(&b, '\f'); advance(p); break;
            case 'n': sb_appendc(&b, '\n'); advance(p); break;
            case 'r': sb_appendc(&b, '\r'); advance(p); break;
            case 't': sb_appendc(&b, '\t'); advance(p); break;
            case 'u': {
                advance(p);
                if (p->pos + 4 > p->len) { jerr(p, "\\u 转义不完整"); sb_free(&b); return NULL; }
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p->s[p->pos];
                    int hv = (h >= '0' && h <= '9') ? h - '0'
                           : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                           : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (hv < 0) { jerr(p, "\\u 转义含非法十六进制"); sb_free(&b); return NULL; }
                    cp = cp * 16 + (unsigned)hv;
                    advance(p);
                }
                /* 代理对。 */
                if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len &&
                    p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    size_t save_pos = p->pos; int save_col = p->col;
                    advance(p); advance(p);
                    unsigned lo = 0; int ok = 1;
                    for (int i = 0; i < 4; i++) {
                        char h = p->s[p->pos];
                        int hv = (h >= '0' && h <= '9') ? h - '0'
                               : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                               : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (hv < 0) { ok = 0; break; }
                        lo = lo * 16 + (unsigned)hv;
                        advance(p);
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p->pos = save_pos; p->col = save_col; cp = 0xFFFD;
                    }
                }
                /* 编码为 UTF-8。 */
                if (cp < 0x80) sb_appendc(&b, (char)cp);
                else if (cp < 0x800) {
                    sb_appendc(&b, (char)(0xC0 | (cp >> 6)));
                    sb_appendc(&b, (char)(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    sb_appendc(&b, (char)(0xE0 | (cp >> 12)));
                    sb_appendc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    sb_appendc(&b, (char)(0x80 | (cp & 0x3F)));
                } else {
                    sb_appendc(&b, (char)(0xF0 | (cp >> 18)));
                    sb_appendc(&b, (char)(0x80 | ((cp >> 12) & 0x3F)));
                    sb_appendc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    sb_appendc(&b, (char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                jerr(p, "未知转义字符 \\%c", e);
                sb_free(&b);
                return NULL;
            }
            continue;
        }
        if (c < 0x20) { jerr(p, "字符串中出现未转义的控制字符"); sb_free(&b); return NULL; }
        sb_appendc(&b, (char)c);
        advance(p);
    }
    jerr(p, "字符串未闭合");
    sb_free(&b);
    return NULL;
}

static jv *parse_number(jparse *p)
{
    size_t start = p->pos;
    if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) advance(p);
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) advance(p);
    if (p->pos < p->len && p->s[p->pos] == '.') {
        advance(p);
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) advance(p);
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        advance(p);
        if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) advance(p);
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) advance(p);
    }
    if (p->pos == start) { jerr(p, "期望数字"); return NULL; }
    char tmp[64];
    size_t n = p->pos - start;
    if (n >= sizeof(tmp)) { jerr(p, "数字过长"); return NULL; }
    memcpy(tmp, p->s + start, n);
    tmp[n] = '\0';
    jv *v = jv_new(JV_NUM);
    v->u.num = strtod(tmp, NULL);
    return v;
}

static jv *parse_value(jparse *p, int depth)
{
    if (depth > 32) { jerr(p, "嵌套层级过深"); return NULL; }
    skip_ws(p);
    if (p->pos >= p->len) { jerr(p, "意外结束"); return NULL; }
    char c = p->s[p->pos];
    if (c == '"') return parse_string(p);
    if (c == '{') {
        advance(p);
        jv *o = jv_new(JV_OBJ);
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') { advance(p); return o; }
        for (;;) {
            skip_ws(p);
            jv *k = parse_string(p);
            if (!k) { jv_free(o); return NULL; }
            skip_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') {
                jerr(p, "对象成员缺少冒号");
                jv_free(k); jv_free(o);
                return NULL;
            }
            advance(p);
            jv *val = parse_value(p, depth + 1);
            if (!val) { jv_free(k); jv_free(o); return NULL; }
            if (o->u.obj.n == o->u.obj.cap) {
                o->u.obj.cap = o->u.obj.cap ? o->u.obj.cap * 2 : 8;
                o->u.obj.keys = (char **)dsc_xrealloc(o->u.obj.keys, o->u.obj.cap * sizeof(char *));
                o->u.obj.vals = (jv **)dsc_xrealloc(o->u.obj.vals, o->u.obj.cap * sizeof(jv *));
            }
            o->u.obj.keys[o->u.obj.n] = dsc_strdup(k->u.str);
            o->u.obj.vals[o->u.obj.n] = val;
            o->u.obj.n++;
            jv_free(k);
            skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { advance(p); continue; }
            if (p->pos < p->len && p->s[p->pos] == '}') { advance(p); return o; }
            jerr(p, "对象成员之间缺少逗号或右括号");
            jv_free(o);
            return NULL;
        }
    }
    if (c == '[') {
        advance(p);
        jv *a = jv_new(JV_ARR);
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') { advance(p); return a; }
        for (;;) {
            jv *val = parse_value(p, depth + 1);
            if (!val) { jv_free(a); return NULL; }
            if (a->u.arr.n == a->u.arr.cap) {
                a->u.arr.cap = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
                a->u.arr.items = (jv **)dsc_xrealloc(a->u.arr.items, a->u.arr.cap * sizeof(jv *));
            }
            a->u.arr.items[a->u.arr.n++] = val;
            skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { advance(p); continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') { advance(p); return a; }
            jerr(p, "数组元素之间缺少逗号或右方括号");
            jv_free(a);
            return NULL;
        }
    }
    if (c == 't' && p->pos + 4 <= p->len && strncmp(p->s + p->pos, "true", 4) == 0) {
        for (int i = 0; i < 4; i++) advance(p);
        jv *v = jv_new(JV_BOOL);
        v->u.b = 1;
        return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && strncmp(p->s + p->pos, "false", 5) == 0) {
        for (int i = 0; i < 5; i++) advance(p);
        jv *v = jv_new(JV_BOOL);
        v->u.b = 0;
        return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && strncmp(p->s + p->pos, "null", 4) == 0) {
        for (int i = 0; i < 4; i++) advance(p);
        return jv_new(JV_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(p);
    jerr(p, "无法识别的值起始字符 '%c'", c);
    return NULL;
}

int jv_parse(const char *text, size_t len, jv **out, char *err, size_t errlen)
{
    if (!text || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    jparse p;
    memset(&p, 0, sizeof(p));
    p.s = text;
    p.len = len ? len : strlen(text);
    p.line = 1;
    p.col = 1;
    p.err = err;
    p.errlen = errlen;
    jv *v = parse_value(&p, 0);
    if (!v) return p.failed ? DSC_E_FORMAT : DSC_E_INVAL;
    skip_ws(&p);
    if (p.pos != p.len) {
        jerr(&p, "JSON 结束后存在多余内容");
        jv_free(v);
        return DSC_E_FORMAT;
    }
    *out = v;
    return DSC_OK;
}

/* ================= 序列化（jv → 文本） ================= */
static void ser(sb *b, const jv *v)
{
    if (!v) { sb_append(b, "null"); return; }
    switch (v->type) {
    case JV_NULL: sb_append(b, "null"); break;
    case JV_BOOL: sb_append(b, v->u.b ? "true" : "false"); break;
    case JV_NUM:  sb_appendf(b, "%.17g", v->u.num); break;
    case JV_STR:  jw_escape_into(b, v->u.str ? v->u.str : ""); break;
    case JV_ARR:
        sb_appendc(b, '[');
        for (size_t i = 0; i < v->u.arr.n; i++) {
            if (i) sb_appendc(b, ',');
            ser(b, v->u.arr.items[i]);
        }
        sb_appendc(b, ']');
        break;
    case JV_OBJ:
        sb_appendc(b, '{');
        for (size_t i = 0; i < v->u.obj.n; i++) {
            if (i) sb_appendc(b, ',');
            jw_escape_into(b, v->u.obj.keys[i] ? v->u.obj.keys[i] : "");
            sb_appendc(b, ':');
            ser(b, v->u.obj.vals[i]);
        }
        sb_appendc(b, '}');
        break;
    default: sb_append(b, "null"); break;
    }
}

void jv_serialize(const jv *v, sb *out)
{
    if (!out) return;
    ser(out, v);
}
