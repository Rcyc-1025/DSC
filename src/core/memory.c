/* memory.c —— AI 记忆抽取、关键词索引与检索（§9.2 / FR-36）。 */
#include "memory.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "diaglog.h"

/* 不参与索引的两字组合（常见虚词）。 */
static const char *g_stop2[] = {
    "的时", "我们", "你们", "他们", "这个", "那个", "就是", "可以", "什么", "没有",
    "一个", "不是", "因为", "所以", "但是", "如果", "这样", "那样", "还是", "已经",
    "自己", "现在", "知道", "觉得", "应该", "需要", "进行", "通过", "以及", "或者",
    "并且", "然后", "其实", "可能", "一定", "非常", "比较", "一些", "这些", "那些",
    "如何", "时候", "的话", "一样", "有点"
};
static const char *g_stop_en[] = {
    "the", "and", "for", "are", "but", "not", "you", "all", "can", "her", "was",
    "one", "our", "out", "day", "get", "has", "him", "his", "how", "its", "new",
    "now", "old", "see", "two", "way", "who", "boy", "did", "she", "use"
};

static int is_stop2(const char *two)
{
    for (size_t i = 0; i < sizeof(g_stop2) / sizeof(g_stop2[0]); i++) {
        if (strcmp(g_stop2[i], two) == 0) return 1;
    }
    return 0;
}

static int is_stop_en(const char *w)
{
    for (size_t i = 0; i < sizeof(g_stop_en) / sizeof(g_stop_en[0]); i++) {
        if (strcmp(g_stop_en[i], w) == 0) return 1;
    }
    return 0;
}

/* UTF-8 三字节字符起始（CJK 落在 U+4E00..U+9FFF，首字节 0xE4..0xE9）。 */
static int cjk_at(const char *s, size_t *len)
{
    unsigned char c = (unsigned char)s[0];
    if (c >= 0xE4 && c <= 0xE9 && s[1] && s[2]) {
        if (len) *len = 3;
        return 1;
    }
    return 0;
}

/* ---------- 抽取 ---------- */

int aic_mem_sentence_keep(const char *sentence)
{
    if (!sentence) return 0;
    const char *p = sentence;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t chars = aic_utf8_length(p);
    if (chars < 8 || chars > 200) return 0;
    /* 客套与拒答不进记忆。 */
    static const char *skip_head[] = {
        "抱歉", "对不起", "作为一", "我不能", "我无法", "很抱歉", "不好意思", "你可以", "我可以帮"
    };
    for (size_t i = 0; i < sizeof(skip_head) / sizeof(skip_head[0]); i++) {
        size_t n = strlen(skip_head[i]);
        if (strncmp(p, skip_head[i], n) == 0) return 0;
    }
    /* 纯符号 / 代码块 / 表格不进记忆。 */
    if (strstr(p, "\x60\x60\x60") != 0) return 0;
    size_t cjk = 0, alnum = 0;
    for (const char *q = p; *q; ) {
        size_t len = 0;
        if (cjk_at(q, &len)) { cjk++; q += len; continue; }
        if (isalnum((unsigned char)*q)) alnum++;
        q++;
    }
    if (cjk < 6 && alnum < 12) return 0;
    return 1;
}

static int is_sentence_end(const char *p, size_t *adv)
{
    static const char *ends[] = { "。", "！", "？", "；", "!", "?", ";", "\n" };
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        size_t n = strlen(ends[i]);
        if (strncmp(p, ends[i], n) == 0) { if (adv) *adv = n; return 1; }
    }
    if (p[0] == '.' && (p[1] == ' ' || p[1] == '\n' || p[1] == 0)) {
        if (adv) *adv = 1;
        return 1;
    }
    return 0;
}

static void push_entry(aic_db *db, const char *src, const char *conv, const char *sentence,
                       size_t len, const char *origin, int *added);

static void extract_into(aic_db *db, const char *src, const char *conv, const char *content,
                         const char *origin, int *added)
{
    if (!content) return;
    const char *seg = content;
    const char *p = content;
    int count = 0;
    while (count < AIC_MEM_MAX_ENTRIES) {
        size_t adv = 0;
        if (*p && !is_sentence_end(p, &adv)) { p++; continue; }
        size_t len = (size_t)(p - seg);
        if (len > 0) {
            char tmp[AIC_MEM_CONTENT_MAX];
            size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
            memcpy(tmp, seg, n);
            tmp[n] = 0;
            if (aic_mem_sentence_keep(tmp)) {
                char *start = tmp;
                while (*start == ' ' || *start == '\t') start++;
                push_entry(db, src, conv, start, strlen(start), origin, added);
                count++;
            }
        }
        if (!*p) break;
        p += adv;
        seg = p;
    }
}

/* ---------- 关键词 ---------- */

static void index_tokens(aic_db *db, const char *entry_id, const char *text)
{
    if (!db || !entry_id || !text) return;
    aic_stmt *st = aic_db_prepare(db,
        "INSERT OR IGNORE INTO memory_token (entry_id, token) VALUES (?, ?);");
    if (!st) return;

    const char *p = text;
    char prev_cjk[4] = { 0 };
    int have_prev = 0;
    while (*p) {
        size_t len = 0;
        if (cjk_at(p, &len)) {
            char cur[4] = { 0 };
            memcpy(cur, p, 3);
            if (have_prev) {
                char two[8];
                snprintf(two, sizeof(two), "%s%s", prev_cjk, cur);
                if (!is_stop2(two)) {
                    aic_stmt_bind_text(st, 1, entry_id);
                    aic_stmt_bind_text(st, 2, two);
                    aic_stmt_step(st);
                    aic_stmt_reset(st);
                }
            }
            memcpy(prev_cjk, cur, 3);
            prev_cjk[3] = 0;
            have_prev = 1;
            p += len;
            continue;
        }
        if (isalnum((unsigned char)*p)) {
            const char *q = p;
            char word[64];
            size_t n = 0;
            while (isalnum((unsigned char)*q) || *q == '-' || *q == '_') {
                if (n + 1 < sizeof(word)) word[n++] = (char)tolower((unsigned char)*q);
                q++;
            }
            word[n] = 0;
            if (n >= 2 && !is_stop_en(word)) {
                aic_stmt_bind_text(st, 1, entry_id);
                aic_stmt_bind_text(st, 2, word);
                aic_stmt_step(st);
                aic_stmt_reset(st);
            }
            p = q;
            have_prev = 0;
            continue;
        }
        have_prev = 0;
        p++;
    }
    aic_stmt_finalize(st);
}

static void tokenize_all(const char *text, sb *out, const char *sep)
{
    if (!text) return;
    sb_reset(out);
    const char *p = text;
    int have_prev = 0;
    char prev_cjk[4] = { 0 };
    while (*p) {
        size_t len = 0;
        if (cjk_at(p, &len)) {
            char cur[4] = { 0 };
            memcpy(cur, p, 3);
            if (have_prev) {
                char two[8];
                snprintf(two, sizeof(two), "%s%s", prev_cjk, cur);
                if (!is_stop2(two)) { if (out->len) sb_append(out, sep); sb_append(out, two); }
            }
            memcpy(prev_cjk, cur, 3);
            prev_cjk[3] = 0;
            have_prev = 1;
            p += len;
            continue;
        }
        if (isalnum((unsigned char)*p)) {
            const char *q = p;
            char word[64];
            size_t n = 0;
            while (isalnum((unsigned char)*q) || *q == '-' || *q == '_') {
                if (n + 1 < sizeof(word)) word[n++] = (char)tolower((unsigned char)*q);
                q++;
            }
            word[n] = 0;
            if (n >= 2 && !is_stop_en(word)) { if (out->len) sb_append(out, sep); sb_append(out, word); }
            p = q;
            have_prev = 0;
            continue;
        }
        have_prev = 0;
        p++;
    }
}

/* ---------- 写入 ---------- */

static void push_entry(aic_db *db, const char *src, const char *conv, const char *sentence,
                       size_t len, const char *origin, int *added)
{
    aic_mem_entry e;
    memset(&e, 0, sizeof(e));
    aic_uuid4(e.id);
    aic_strlcpy_utf8(e.source_message_id, sizeof(e.source_message_id), src ? src : "");
    aic_strlcpy_utf8(e.conversation_id, sizeof(e.conversation_id), conv ? conv : "");
    size_t n = len < AIC_MEM_CONTENT_MAX - 1 ? len : AIC_MEM_CONTENT_MAX - 1;
    memcpy(e.content, sentence, n);
    e.content[n] = 0;
    aic_strlcpy_utf8(e.origin_device, sizeof(e.origin_device), origin ? origin : "");
    aic_now_iso8601(e.created_at);
    aic_strlcpy_utf8(e.updated_at, sizeof(e.updated_at), e.created_at);

    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO memory_entry (id, source_message_id, conversation_id, content,"
        " origin_device, created_at, updated_at) VALUES (?,?,?,?,?,?,?);");
    if (!st) return;
    aic_stmt_bind_text(st, 1, e.id);
    aic_stmt_bind_text(st, 2, e.source_message_id);
    aic_stmt_bind_text(st, 3, e.conversation_id);
    aic_stmt_bind_text(st, 4, e.content);
    aic_stmt_bind_text(st, 5, e.origin_device);
    aic_stmt_bind_text(st, 6, e.created_at);
    aic_stmt_bind_text(st, 7, e.updated_at);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return;
    index_tokens(db, e.id, sentence);
    if (added) (*added)++;
}

int aic_mem_purge(aic_db *db, const char *source_message_id, int *out_removed)
{
    if (out_removed) *out_removed = 0;
    if (!db || !source_message_id) return AIC_E_INVAL;
    int before = aic_mem_count_for(db, source_message_id);
    aic_stmt *st = aic_db_prepare(db,
        "DELETE FROM memory_token WHERE entry_id IN"
        " (SELECT id FROM memory_entry WHERE source_message_id = ?);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, source_message_id);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;

    st = aic_db_prepare(db, "DELETE FROM memory_entry WHERE source_message_id = ?;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, source_message_id);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    if (out_removed) *out_removed = before;
    return AIC_OK;
}

int aic_mem_extract(aic_db *db, const char *source_message_id, const char *conversation_id,
                    const char *content, const char *origin_device, int *out_added)
{
    if (out_added) *out_added = 0;
    if (!db || !source_message_id) return AIC_E_INVAL;
    if (!content || !content[0]) return AIC_OK;
    int added = 0;
    extract_into(db, source_message_id, conversation_id, content, origin_device, &added);
    if (out_added) *out_added = added;
    return AIC_OK;
}

int aic_mem_rebuild(aic_db *db, const char *source_message_id, const char *conversation_id,
                    const char *content, const char *origin_device, int *out_added)
{
    if (out_added) *out_added = 0;
    if (!db || !source_message_id) return AIC_E_INVAL;
    int rc = aic_db_begin(db);
    if (rc != AIC_OK) return rc;
    int removed = 0;
    rc = aic_mem_purge(db, source_message_id, &removed);
    if (rc == AIC_OK) rc = aic_mem_extract(db, source_message_id, conversation_id, content,
                                           origin_device, out_added);
    if (rc != AIC_OK) { aic_db_rollback(db); return rc; }
    return aic_db_commit(db);
}

/* ---------- 查询 ---------- */

int aic_mem_count(aic_db *db)
{
    if (!db) return 0;
    aic_stmt *st = aic_db_prepare(db, "SELECT COUNT(*) FROM memory_entry;");
    if (!st) return 0;
    int n = 0;
    if (aic_stmt_step(st) == AIC_ROW) n = aic_stmt_int(st, 0);
    aic_stmt_finalize(st);
    return n;
}

int aic_mem_count_for(aic_db *db, const char *source_message_id)
{
    if (!db || !source_message_id) return 0;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT COUNT(*) FROM memory_entry WHERE source_message_id = ?;");
    if (!st) return 0;
    aic_stmt_bind_text(st, 1, source_message_id);
    int n = 0;
    if (aic_stmt_step(st) == AIC_ROW) n = aic_stmt_int(st, 0);
    aic_stmt_finalize(st);
    return n;
}

static void row_to_mem(aic_stmt *st, aic_mem_entry *e)
{
    memset(e, 0, sizeof(*e));
    aic_strlcpy_utf8(e->id, sizeof(e->id), aic_stmt_text(st, 0));
    aic_strlcpy_utf8(e->source_message_id, sizeof(e->source_message_id), aic_stmt_text(st, 1));
    aic_strlcpy_utf8(e->conversation_id, sizeof(e->conversation_id), aic_stmt_text(st, 2));
    aic_strlcpy_utf8(e->content, sizeof(e->content), aic_stmt_text(st, 3));
    aic_strlcpy_utf8(e->origin_device, sizeof(e->origin_device), aic_stmt_text(st, 4));
    aic_strlcpy_utf8(e->created_at, sizeof(e->created_at), aic_stmt_text(st, 5));
    aic_strlcpy_utf8(e->updated_at, sizeof(e->updated_at), aic_stmt_text(st, 6));
}

#define MEM_COLS "id, source_message_id, conversation_id, content, origin_device, created_at, updated_at"

int aic_mem_list(aic_db *db, const char *source_message_id, aic_mem_entry *out, int cap, int *out_n)
{
    if (out_n) *out_n = 0;
    if (!db || !out || cap <= 0) return AIC_E_INVAL;
    aic_stmt *st;
    if (source_message_id && source_message_id[0]) {
        st = aic_db_prepare(db, "SELECT " MEM_COLS " FROM memory_entry"
                                 " WHERE source_message_id = ? ORDER BY created_at, id;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, source_message_id);
    } else {
        st = aic_db_prepare(db, "SELECT " MEM_COLS " FROM memory_entry"
                                 " ORDER BY created_at DESC, id LIMIT ?;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_ll(st, 1, cap);
    }
    int n = 0, rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n < cap) row_to_mem(st, &out[n]);
        n++;
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    if (out_n) *out_n = n;
    return AIC_OK;
}

int aic_mem_search(aic_db *db, const char *query, aic_mem_entry *out, int cap, int *out_n)
{
    if (out_n) *out_n = 0;
    if (!db || !out || cap <= 0) return AIC_E_INVAL;
    if (!query || !query[0]) return aic_mem_list(db, NULL, out, cap, out_n);

    sb toks;
    sb_init(&toks);
    tokenize_all(query, &toks, "','");
    if (toks.len == 0) { sb_free(&toks); return aic_mem_list(db, NULL, out, cap, out_n); }

    sb sql;
    sb_init(&sql);
    sb_append(&sql, "SELECT e.id, e.source_message_id, e.conversation_id, e.content,"
                    " e.origin_device, e.created_at, e.updated_at, COUNT(*) AS hits"
                    " FROM memory_entry e JOIN memory_token t ON t.entry_id = e.id"
                    " WHERE t.token IN ('");
    sb_append(&sql, sb_cstr(&toks));
    sb_append(&sql, "') GROUP BY e.id ORDER BY hits DESC, e.created_at DESC LIMIT ");
    sb_appendf(&sql, "%d;", cap);
    sb_free(&toks);

    aic_stmt *st = aic_db_prepare(db, sb_cstr(&sql));
    sb_free(&sql);
    if (!st) return AIC_E_DB;
    int n = 0, rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n < cap) row_to_mem(st, &out[n]);
        n++;
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    if (out_n) *out_n = n;
    return AIC_OK;
}

int aic_mem_context(aic_db *db, const char *conversation_id, sb *out)
{
    if (!out) return AIC_E_INVAL;
    sb_reset(out);
    if (!db) return AIC_E_INVAL;
    aic_stmt *st;
    if (conversation_id && conversation_id[0]) {
        st = aic_db_prepare(db, "SELECT " MEM_COLS " FROM memory_entry"
                                 " WHERE conversation_id = ? ORDER BY created_at DESC, id LIMIT 24;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, conversation_id);
    } else {
        st = aic_db_prepare(db, "SELECT " MEM_COLS " FROM memory_entry"
                                 " ORDER BY created_at DESC, id LIMIT 24;");
        if (!st) return AIC_E_DB;
    }
    int rc, count = 0;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (out->len > AIC_MEM_CONTEXT_MAX) break;
        const char *text = aic_stmt_text(st, 3);
        if (!text || !text[0]) continue;
        if (count == 0) sb_append(out, "【AI 记忆（来自本会话的既成发言，可直接引用）】\n");
        sb_appendf(out, "- %s\n", text);
        count++;
    }
    aic_stmt_finalize(st);
    return AIC_OK;
}

int aic_mem_clear(aic_db *db)
{
    if (!db) return AIC_E_INVAL;
    int rc = aic_db_exec(db, "DELETE FROM memory_token;");
    if (rc != AIC_OK) return rc;
    return aic_db_exec(db, "DELETE FROM memory_entry;");
}
