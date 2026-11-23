/* transfer.c —— 聊天记录导出/导入实现。 */
#include "transfer.h"
#include "store_conversation.h"
#include "store_profile.h"
#include "store_message.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 内存中的传输结构 ---------- */
typedef struct tr_conv {
    char id[AIC_ID_BYTES];
    char mode[8];
    char title[AIC_TITLE_BYTES];
    char persona_id[AIC_ID_BYTES];
    char model_id[AIC_MODEL_BYTES];
    int  pinned;
    long long last_seq;
    char created_at[AIC_TS_BYTES];
    char updated_at[AIC_TS_BYTES];
} tr_conv;

typedef struct tr_msg {
    char  id[40];
    char  conversation_id[40];
    long long seq;
    char  role[16];
    char *content;
    int   recalled;
    int   superseded;
    char  edited_at[32];
    char *original_content;
    char  model_id[64];
    char  created_at[32];
    long long logical_ts;      /* 跨端时序（§16.5），导出时保留原值 */
    char  origin_device[40];   /* 消息产生端设备标识 */
} tr_msg;

typedef struct tr_doc {
    int    schema_version;
    char   exported_at[32];
    char   source_mode[8];
    char   checksum[65];
    tr_conv *convs;
    size_t   conv_n;
    tr_msg  *msgs;
    size_t   msg_n;
} tr_doc;

static void tr_doc_free(tr_doc *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->msg_n; i++) {
        aic_xfree(d->msgs[i].content);
        aic_xfree(d->msgs[i].original_content);
    }
    aic_xfree(d->msgs);
    aic_xfree(d->convs);
    memset(d, 0, sizeof(*d));
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* ---------- 校验和 ---------- */
static void checksum_feed(aic_sha256_ctx *c, const tr_msg *m)
{
    const unsigned char sep = 0x1F;
    aic_sha256_update(c, m->id, strlen(m->id));
    aic_sha256_update(c, &sep, 1);
    const char *body = m->content ? m->content : "";
    aic_sha256_update(c, body, strlen(body));
    aic_sha256_update(c, &sep, 1);
}

static void checksum_of(const tr_msg *msgs, size_t n, char out[65])
{
    aic_sha256_ctx c;
    aic_sha256_init(&c);
    for (size_t i = 0; i < n; i++) checksum_feed(&c, &msgs[i]);
    unsigned char digest[32];
    aic_sha256_final(&c, digest);
    aic_hex_encode(digest, 32, out);
}

/* ---------- 导出 ---------- */
/* scope=2 的时间区间转成 ISO 文本，与 message.created_at 做字典序比较（同为 UTC 定长格式）。 */
static void scope_bounds(const aic_export_scope *sc, char *from_iso, char *to_iso)
{
    from_iso[0] = '\0';
    to_iso[0] = '\0';
    if (!sc || sc->scope != 2) return;
    if (sc->from_ms > 0) aic_ms_to_iso8601(sc->from_ms, from_iso);
    if (sc->to_ms > 0) aic_ms_to_iso8601(sc->to_ms, to_iso);
}

static int collect(aic_db *db, int mode, const aic_export_scope *sc, tr_doc *doc,
                   char *err, size_t errlen)
{
    aic_conversation *convs = NULL;
    size_t conv_n = 0;
    int rc = aic_conv_list(db, mode, &convs, &conv_n);
    if (rc != AIC_OK) { set_err(err, errlen, "读取会话失败：%s", aic_strerror(rc)); return rc; }

    doc->convs = aic_xcalloc(conv_n ? conv_n : 1, sizeof(tr_conv));
    doc->conv_n = 0;
    size_t msg_cap = 64, msg_n = 0;
    doc->msgs = aic_xcalloc(msg_cap, sizeof(tr_msg));

    char from_iso[32], to_iso[32];
    scope_bounds(sc, from_iso, to_iso);

    for (size_t i = 0; i < conv_n; i++) {
        if (sc && sc->scope == 1) {
            if (!sc->conv_id || strcmp(convs[i].id, sc->conv_id) != 0) continue;
        }
        tr_conv *tc = &doc->convs[doc->conv_n++];
        aic_strlcpy_utf8(tc->id, sizeof(tc->id), convs[i].id);
        aic_strlcpy_utf8(tc->mode, sizeof(tc->mode), convs[i].mode);
        aic_strlcpy_utf8(tc->title, sizeof(tc->title), convs[i].title);
        aic_strlcpy_utf8(tc->persona_id, sizeof(tc->persona_id), convs[i].persona_id);
        aic_strlcpy_utf8(tc->model_id, sizeof(tc->model_id), convs[i].model_id);
        tc->pinned = convs[i].pinned;
        tc->last_seq = convs[i].last_seq;
        aic_strlcpy_utf8(tc->created_at, sizeof(tc->created_at), convs[i].created_at);
        aic_strlcpy_utf8(tc->updated_at, sizeof(tc->updated_at), convs[i].updated_at);

        aic_message *msgs = NULL;
        size_t n = 0;
        rc = aic_msg_export_all(db, convs[i].id, &msgs, &n);
        if (rc != AIC_OK) {
            aic_conv_free(convs, conv_n);
            set_err(err, errlen, "读取消息失败：%s", aic_strerror(rc));
            return rc;
        }
        size_t kept_before = msg_n;
        for (size_t k = 0; k < n; k++) {
            if (sc && sc->scope == 2) {
                const char *ts = msgs[k].created_at;
                if (from_iso[0] && strcmp(ts, from_iso) < 0) continue;
                if (to_iso[0] && strcmp(ts, to_iso) > 0) continue;
            }
            if (msg_n == msg_cap) {
                msg_cap *= 2;
                doc->msgs = aic_xrealloc(doc->msgs, msg_cap * sizeof(tr_msg));
                memset(doc->msgs + msg_n, 0, (msg_cap - msg_n) * sizeof(tr_msg));
            }
            tr_msg *tm = &doc->msgs[msg_n++];
            aic_strlcpy_utf8(tm->id, sizeof(tm->id), msgs[k].id);
            aic_strlcpy_utf8(tm->conversation_id, sizeof(tm->conversation_id), msgs[k].conversation_id);
            tm->seq = msgs[k].seq;
            aic_strlcpy_utf8(tm->role, sizeof(tm->role), msgs[k].role);
            tm->content = aic_strdup(msgs[k].content ? msgs[k].content : "");
            tm->recalled = msgs[k].recalled;
            tm->superseded = msgs[k].superseded;
            aic_strlcpy_utf8(tm->edited_at, sizeof(tm->edited_at), msgs[k].edited_at);
            tm->original_content = msgs[k].original_content ? aic_strdup(msgs[k].original_content) : NULL;
            aic_strlcpy_utf8(tm->model_id, sizeof(tm->model_id), msgs[k].model_id);
            aic_strlcpy_utf8(tm->created_at, sizeof(tm->created_at), msgs[k].created_at);
            tm->logical_ts = msgs[k].logical_ts;
            aic_strlcpy_utf8(tm->origin_device, sizeof(tm->origin_device), msgs[k].origin_device);
        }
        aic_msg_free_array(msgs, n);
        if (sc && sc->scope == 2 && msg_n == kept_before) doc->conv_n--;  /* 区间内没有消息的会话不导出 */
    }
    doc->msg_n = msg_n;
    aic_conv_free(convs, conv_n);
    return AIC_OK;
}

static void emit_avatars(aic_db *db, jw *w, const aic_export_scope *sc)
{
    jw_arr_begin(w, "avatars");
    if (!sc || !sc->include_avatars || !sc->root || !sc->root[0]) { jw_arr_end(w); return; }
    aic_profile p;
    memset(&p, 0, sizeof(p));
    if (aic_profile_get(db, &p) != AIC_OK || !p.avatar_path[0]) { jw_arr_end(w); return; }
    char full[1024];
    if (aic_path_join(full, sizeof(full), sc->root, p.avatar_path) != AIC_OK) { jw_arr_end(w); return; }
    char *data = NULL;
    size_t len = 0;
    if (aic_file_read_all(full, &data, &len) != AIC_OK) { jw_arr_end(w); return; }
    char sha[65];
    aic_sha256_hex(data, len, sha);
    sb b64;
    sb_init(&b64);
    aic_base64_encode(data, len, &b64);
    aic_xfree(data);
    jw_obj_begin(w, NULL);
    jw_put_str(w, "path", p.avatar_path);
    jw_put_str(w, "sha256", sha);
    jw_put_str(w, "data_base64", sb_cstr(&b64));
    jw_obj_end(w);
    sb_free(&b64);
    jw_arr_end(w);
}

int aic_export_json(aic_db *db, int mode, sb *out, char *err, size_t errlen)
{
    return aic_export_json_ex(db, mode, NULL, out, err, errlen);
}

int aic_export_json_ex(aic_db *db, int mode, const aic_export_scope *sc, sb *out,
                       char *err, size_t errlen)
{
    if (!db || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    tr_doc doc;
    memset(&doc, 0, sizeof(doc));
    doc.schema_version = AIC_EXPORT_SCHEMA_VER;
    aic_now_iso8601(doc.exported_at);
    if (mode == AIC_MODE_QA) aic_strlcpy_utf8(doc.source_mode, sizeof(doc.source_mode), "qa");
    else if (mode == AIC_MODE_CHAT) aic_strlcpy_utf8(doc.source_mode, sizeof(doc.source_mode), "chat");
    else aic_strlcpy_utf8(doc.source_mode, sizeof(doc.source_mode), "all");

    int rc = collect(db, mode, sc, &doc, err, errlen);
    if (rc != AIC_OK) { tr_doc_free(&doc); return rc; }
    checksum_of(doc.msgs, doc.msg_n, doc.checksum);

    jw w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "format", AIC_EXPORT_FORMAT);
    jw_put_int(&w, "schema_version", doc.schema_version);
    jw_put_str(&w, "exported_at", doc.exported_at);
    jw_put_str(&w, "source_mode", doc.source_mode);

    jw_arr_begin(&w, "conversations");
    for (size_t i = 0; i < doc.conv_n; i++) {
        const tr_conv *c = &doc.convs[i];
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "id", c->id);
        jw_put_str(&w, "mode", c->mode);
        jw_put_str(&w, "title", c->title);
        jw_put_str(&w, "persona_id", c->persona_id);
        jw_put_str(&w, "model_id", c->model_id);
        jw_put_int(&w, "pinned", c->pinned);
        jw_put_int(&w, "last_seq", c->last_seq);
        jw_put_str(&w, "created_at", c->created_at);
        jw_put_str(&w, "updated_at", c->updated_at);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);

    jw_arr_begin(&w, "messages");
    for (size_t i = 0; i < doc.msg_n; i++) {
        const tr_msg *m = &doc.msgs[i];
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "id", m->id);
        jw_put_str(&w, "conversation_id", m->conversation_id);
        jw_put_int(&w, "seq", m->seq);
        jw_put_str(&w, "role", m->role);
        jw_put_str(&w, "content", m->content ? m->content : "");
        jw_put_int(&w, "recalled", m->recalled);
        jw_put_int(&w, "superseded", m->superseded);
        jw_put_str(&w, "edited_at", m->edited_at);
        if (m->original_content) jw_put_str(&w, "original_content", m->original_content);
        else jw_put_null(&w, "original_content");
        jw_put_str(&w, "model_id", m->model_id);
        jw_put_str(&w, "created_at", m->created_at);
        jw_put_int(&w, "logical_ts", m->logical_ts);
        jw_put_str(&w, "origin_device", m->origin_device);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);

    emit_avatars(db, &w, sc);
    jw_put_str(&w, "checksum", doc.checksum);
    jw_obj_end(&w);
    tr_doc_free(&doc);
    return AIC_OK;
}

static int write_file(const char *path, sb *content, char *err, size_t errlen)
{
    int rc = aic_file_write_all(path, sb_cstr(content), content->len);
    if (rc != AIC_OK) set_err(err, errlen, "写入文件失败：%s", aic_strerror(rc));
    return rc;
}

int aic_export_write_json_ex(aic_db *db, int mode, const aic_export_scope *sc, const char *path,
                             char *err, size_t errlen)
{
    if (!path) return AIC_E_INVAL;
    sb b;
    sb_init(&b);
    int rc = aic_export_json_ex(db, mode, sc, &b, err, errlen);
    if (rc == AIC_OK) rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

int aic_export_write_json(aic_db *db, int mode, const char *path, char *err, size_t errlen)
{
    if (!path) return AIC_E_INVAL;
    sb b;
    sb_init(&b);
    int rc = aic_export_json(db, mode, &b, err, errlen);
    if (rc == AIC_OK) rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

static void write_conv_body(aic_db *db, int mode, const aic_export_scope *sc, sb *out,
                            int markdown, char *err, size_t errlen)
{
    aic_conversation *convs = NULL;
    size_t n = 0;
    char from_iso[32], to_iso[32];
    scope_bounds(sc, from_iso, to_iso);
    if (aic_conv_list(db, mode, &convs, &n) != AIC_OK) {
        set_err(err, errlen, "读取会话失败");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const aic_conversation *c = &convs[i];
        size_t written = 0;
        if (sc && sc->scope == 1) {
            if (!sc->conv_id || strcmp(c->id, sc->conv_id) != 0) continue;
        }
        if (markdown) sb_appendf(out, "# %s（%s）\n\n", c->title, c->mode);
        else sb_appendf(out, "===== %s（%s） =====\n\n", c->title, c->mode);
        aic_message *msgs = NULL;
        size_t mn = 0;
        if (aic_msg_export_all(db, c->id, &msgs, &mn) == AIC_OK) {
            for (size_t k = 0; k < mn; k++) {
                if (sc && sc->scope == 2) {
                    const char *ts = msgs[k].created_at;
                    if (from_iso[0] && strcmp(ts, from_iso) < 0) continue;
                    if (to_iso[0] && strcmp(ts, to_iso) > 0) continue;
                }
                written++;
                const char *who = strcmp(msgs[k].role, "user") == 0 ? "用户"
                                : (strcmp(msgs[k].role, "system") == 0 ? "系统" : "助手");
                if (msgs[k].recalled) {
                    sb_appendf(out, "[%s] %s\n\n", who, AIC_RECALL_NOTICE);
                } else if (markdown) {
                    sb_appendf(out, "**%s**：%s\n\n", who, msgs[k].content ? msgs[k].content : "");
                } else {
                    sb_appendf(out, "%s：%s\n\n", who, msgs[k].content ? msgs[k].content : "");
                }
            }
            aic_msg_free_array(msgs, mn);
        }
        if (sc && sc->scope == 2 && written == 0) continue;   /* 区间内没有消息的会话不导出 */
        if (markdown) sb_append(out, "---\n\n");
    }
    aic_conv_free(convs, n);
}

int aic_export_write_markdown_ex(aic_db *db, int mode, const aic_export_scope *sc, const char *path,
                                 char *err, size_t errlen)
{
    if (!db || !path) return AIC_E_INVAL;
    sb b;
    sb_init(&b);
    sb_append(&b, "# Sai 聊天记录导出\n\n");
    char ts[32];
    aic_now_iso8601(ts);
    sb_appendf(&b, "导出时间：%s\n\n", ts);
    write_conv_body(db, mode, sc, &b, 1, err, errlen);
    int rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

int aic_export_write_markdown(aic_db *db, int mode, const char *path, char *err, size_t errlen)
{
    return aic_export_write_markdown_ex(db, mode, NULL, path, err, errlen);
}

int aic_export_write_text_ex(aic_db *db, int mode, const aic_export_scope *sc, const char *path,
                             char *err, size_t errlen)
{
    if (!db || !path) return AIC_E_INVAL;
    sb b;
    sb_init(&b);
    sb_append(&b, "Sai 聊天记录导出\n\n");
    write_conv_body(db, mode, sc, &b, 0, err, errlen);
    int rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

int aic_export_write_text(aic_db *db, int mode, const char *path, char *err, size_t errlen)
{
    return aic_export_write_text_ex(db, mode, NULL, path, err, errlen);
}

/* ---------- 读取并校验 ---------- */
static int parse_doc(const char *path, tr_doc *doc, char *err, size_t errlen)
{
    char *text = NULL;
    size_t len = 0;
    int rc = aic_file_read_all(path, &text, &len);
    if (rc != AIC_OK) {
        set_err(err, errlen, "无法读取导入文件：%s", aic_strerror(rc));
        return rc;
    }
    jv *root = NULL;
    rc = jv_parse(text, len, &root, err, errlen);
    aic_xfree(text);
    if (rc != AIC_OK) return rc;

    const char *fmt = jv_get_str(root, "format", "");
    if (strcmp(fmt, AIC_EXPORT_FORMAT) != 0) {
        set_err(err, errlen, "不是聊天记录导出文件（format=%s）", fmt);
        jv_free(root);
        return AIC_E_FORMAT;
    }
    int ver = jv_get_int(root, "schema_version", -1);
    if (ver != AIC_EXPORT_SCHEMA_VER) {
        set_err(err, errlen, "导出文件版本 %d 与当前支持的 %d 不一致", ver, AIC_EXPORT_SCHEMA_VER);
        jv_free(root);
        return AIC_E_VERSION;
    }
    doc->schema_version = ver;
    aic_strlcpy_utf8(doc->exported_at, sizeof(doc->exported_at), jv_get_str(root, "exported_at", ""));
    aic_strlcpy_utf8(doc->source_mode, sizeof(doc->source_mode), jv_get_str(root, "source_mode", "all"));
    aic_strlcpy_utf8(doc->checksum, sizeof(doc->checksum), jv_get_str(root, "checksum", ""));

    const jv *carr = jv_get(root, "conversations");
    const jv *marr = jv_get(root, "messages");
    if (!carr || carr->type != JV_ARR || !marr || marr->type != JV_ARR) {
        set_err(err, errlen, "导出文件缺少 conversations 或 messages 数组");
        jv_free(root);
        return AIC_E_FORMAT;
    }

    size_t cn = jv_len(carr);
    doc->convs = aic_xcalloc(cn ? cn : 1, sizeof(tr_conv));
    doc->conv_n = 0;
    for (size_t i = 0; i < cn; i++) {
        const jv *o = jv_at(carr, i);
        if (!o || o->type != JV_OBJ) continue;
        tr_conv *c = &doc->convs[doc->conv_n++];
        aic_strlcpy_utf8(c->id, sizeof(c->id), jv_get_str(o, "id", ""));
        aic_strlcpy_utf8(c->mode, sizeof(c->mode), jv_get_str(o, "mode", "qa"));
        aic_strlcpy_utf8(c->title, sizeof(c->title), jv_get_str(o, "title", ""));
        aic_strlcpy_utf8(c->persona_id, sizeof(c->persona_id), jv_get_str(o, "persona_id", ""));
        aic_strlcpy_utf8(c->model_id, sizeof(c->model_id), jv_get_str(o, "model_id", AIC_MODEL_FLASH));
        c->pinned = jv_get_int(o, "pinned", 0);
        c->last_seq = jv_get_ll(o, "last_seq", 0);
        aic_strlcpy_utf8(c->created_at, sizeof(c->created_at), jv_get_str(o, "created_at", ""));
        aic_strlcpy_utf8(c->updated_at, sizeof(c->updated_at), jv_get_str(o, "updated_at", ""));
    }

    size_t mn = jv_len(marr);
    doc->msgs = aic_xcalloc(mn ? mn : 1, sizeof(tr_msg));
    doc->msg_n = 0;
    for (size_t i = 0; i < mn; i++) {
        const jv *o = jv_at(marr, i);
        if (!o || o->type != JV_OBJ) continue;
        tr_msg *m = &doc->msgs[doc->msg_n++];
        aic_strlcpy_utf8(m->id, sizeof(m->id), jv_get_str(o, "id", ""));
        aic_strlcpy_utf8(m->conversation_id, sizeof(m->conversation_id), jv_get_str(o, "conversation_id", ""));
        m->seq = jv_get_ll(o, "seq", 0);
        aic_strlcpy_utf8(m->role, sizeof(m->role), jv_get_str(o, "role", "user"));
        m->content = aic_strdup(jv_get_str(o, "content", ""));
        m->recalled = jv_get_int(o, "recalled", 0);
        m->superseded = jv_get_int(o, "superseded", 0);
        aic_strlcpy_utf8(m->edited_at, sizeof(m->edited_at), jv_get_str(o, "edited_at", ""));
        const jv *orig = jv_get(o, "original_content");
        m->original_content = (orig && orig->type == JV_STR) ? aic_strdup(jv_string(orig)) : NULL;
        aic_strlcpy_utf8(m->model_id, sizeof(m->model_id), jv_get_str(o, "model_id", ""));
        aic_strlcpy_utf8(m->created_at, sizeof(m->created_at), jv_get_str(o, "created_at", ""));
        m->logical_ts = jv_get_ll(o, "logical_ts", 0);
        aic_strlcpy_utf8(m->origin_device, sizeof(m->origin_device), jv_get_str(o, "origin_device", ""));
    }
    jv_free(root);

    /* 校验和：以解析后的消息为准重算。 */
    char calc[65];
    checksum_of(doc->msgs, doc->msg_n, calc);
    if (doc->checksum[0] && strcmp(calc, doc->checksum) != 0) {
        set_err(err, errlen, "校验和不匹配：文件可能被改动或损坏");
        tr_doc_free(doc);
        return AIC_E_CHECKSUM;
    }
    return AIC_OK;
}

static int count_conflicts(aic_db *db, const tr_doc *doc, aic_import_preview *out)
{
    size_t cc = 0, mc = 0;
    for (size_t i = 0; i < doc->conv_n; i++) {
        aic_conversation c;
        if (aic_conv_get(db, doc->convs[i].id, &c) == AIC_OK) cc++;
    }
    for (size_t i = 0; i < doc->msg_n; i++) {
        aic_message m;
        if (aic_msg_get(db, doc->msgs[i].id, &m) == AIC_OK) { mc++; aic_msg_free(&m); }
    }
    out->conflict_conversations = cc;
    out->conflict_messages = mc;
    return AIC_OK;
}

int aic_transfer_preview(aic_db *db, const char *path, aic_import_preview *out,
                         char *err, size_t errlen)
{
    if (!db || !path || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));
    tr_doc doc;
    memset(&doc, 0, sizeof(doc));
    int rc = parse_doc(path, &doc, err, errlen);
    if (rc != AIC_OK) { tr_doc_free(&doc); return rc; }
    out->schema_version = doc.schema_version;
    aic_strlcpy_utf8(out->exported_at, sizeof(out->exported_at), doc.exported_at);
    aic_strlcpy_utf8(out->source_mode, sizeof(out->source_mode), doc.source_mode);
    out->conversations = doc.conv_n;
    out->messages = doc.msg_n;
    count_conflicts(db, &doc, out);
    tr_doc_free(&doc);
    return AIC_OK;
}

/* ---------- 导入 ---------- */
static int import_conversation(aic_db *db, const tr_conv *c, aic_conflict_policy policy,
                               aic_import_result *res)
{
    aic_conversation exists;
    int found = (aic_conv_get(db, c->id, &exists) == AIC_OK);
    if (found && policy == AIC_CONFLICT_SKIP) { res->conv_skipped++; return AIC_OK; }

    char id[40];
    aic_strlcpy_utf8(id, sizeof(id), c->id);
    if (found && policy == AIC_CONFLICT_KEEP_BOTH) {
        aic_uuid4(id);
        found = 0;
        res->conv_renamed++;
    }
    const char *mode = strcmp(c->mode, "chat") == 0 ? "chat" : "qa";

    aic_stmt *st;
    if (found) {
        res->conv_overwritten++;
        st = aic_db_prepare(db,
            "UPDATE conversation SET title=?2, persona_id=?3, model_id=?4, pinned=?5, "
            "last_seq=?6, created_at=?7, updated_at=?8 WHERE id=?1;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, id);
        aic_stmt_bind_text(st, 2, c->title);
        aic_stmt_bind_text(st, 3, c->persona_id);
        aic_stmt_bind_text(st, 4, c->model_id);
        aic_stmt_bind_ll(st, 5, c->pinned);
        aic_stmt_bind_ll(st, 6, c->last_seq);
        aic_stmt_bind_text(st, 7, c->created_at);
        aic_stmt_bind_text(st, 8, c->updated_at);
    } else {
        res->conv_added++;
        st = aic_db_prepare(db,
            "INSERT INTO conversation (id, mode, title, persona_id, model_id, pinned, last_seq, created_at, updated_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, id);
        aic_stmt_bind_text(st, 2, mode);
        aic_stmt_bind_text(st, 3, c->title);
        aic_stmt_bind_text(st, 4, c->persona_id);
        aic_stmt_bind_text(st, 5, c->model_id);
        aic_stmt_bind_ll(st, 6, c->pinned);
        aic_stmt_bind_ll(st, 7, c->last_seq);
        aic_stmt_bind_text(st, 8, c->created_at);
        aic_stmt_bind_text(st, 9, c->updated_at);
    }
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int seq_taken(aic_db *db, const char *conv_id, long long seq)
{
    aic_stmt *st = aic_db_prepare(db,
        "SELECT 1 FROM message WHERE conversation_id=?1 AND seq=?2;");
    if (!st) return 0;
    aic_stmt_bind_text(st, 1, conv_id);
    aic_stmt_bind_ll(st, 2, seq);
    int taken = (aic_stmt_step(st) == AIC_ROW);
    aic_stmt_finalize(st);
    return taken;
}

static long long next_seq(aic_db *db, const char *conv_id)
{
    aic_stmt *st = aic_db_prepare(db, "SELECT IFNULL(MAX(seq),0)+1 FROM message WHERE conversation_id=?1;");
    if (!st) return 1;
    aic_stmt_bind_text(st, 1, conv_id);
    long long v = 1;
    if (aic_stmt_step(st) == AIC_ROW) v = aic_stmt_ll(st, 0);
    aic_stmt_finalize(st);
    return v;
}

static int import_message(aic_db *db, const tr_msg *m, aic_conflict_policy policy,
                          aic_import_result *res)
{
    aic_message exists;
    int found = (aic_msg_get(db, m->id, &exists) == AIC_OK);
    if (found) aic_msg_free(&exists);
    if (found && policy == AIC_CONFLICT_SKIP) { res->msg_skipped++; return AIC_OK; }

    char id[40];
    aic_strlcpy_utf8(id, sizeof(id), m->id);
    if (found && policy == AIC_CONFLICT_KEEP_BOTH) {
        aic_uuid4(id);
        found = 0;
        res->msg_renamed++;
    }

    long long seq = m->seq;
    if (found && policy == AIC_CONFLICT_OVERWRITE) {
        res->msg_overwritten++;
        aic_stmt *st = aic_db_prepare(db,
            "UPDATE message SET role=?2, content=?3, recalled=?4, edited_at=?5, original_content=?6, "
            "model_id=?7, created_at=?8, superseded=?9, logical_ts=?10, origin_device=?11 WHERE id=?1;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, id);
        aic_stmt_bind_text(st, 2, m->role);
        aic_stmt_bind_text(st, 3, m->content ? m->content : "");
        aic_stmt_bind_ll(st, 4, m->recalled);
        aic_stmt_bind_text(st, 5, m->edited_at);
        if (m->original_content) aic_stmt_bind_text(st, 6, m->original_content);
        else aic_stmt_bind_null(st, 6);
        aic_stmt_bind_text(st, 7, m->model_id);
        aic_stmt_bind_text(st, 8, m->created_at);
        aic_stmt_bind_ll(st, 9, m->superseded);
        aic_stmt_bind_ll(st, 10, m->logical_ts);
        aic_stmt_bind_text(st, 11, m->origin_device);
        int rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        return rc == AIC_DONE ? AIC_OK : rc;
    }

    /* 新增：seq 被占用时改为追加到末尾，避免唯一索引冲突。 */
    if (seq <= 0 || seq_taken(db, m->conversation_id, seq)) seq = next_seq(db, m->conversation_id);
    res->msg_added++;
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO message (id, conversation_id, seq, role, content, recalled, edited_at, "
        "original_content, model_id, created_at, superseded, logical_ts, origin_device) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, m->conversation_id);
    aic_stmt_bind_ll(st, 3, seq);
    aic_stmt_bind_text(st, 4, m->role);
    aic_stmt_bind_text(st, 5, m->content ? m->content : "");
    aic_stmt_bind_ll(st, 6, m->recalled);
    aic_stmt_bind_text(st, 7, m->edited_at);
    if (m->original_content) aic_stmt_bind_text(st, 8, m->original_content);
    else aic_stmt_bind_null(st, 8);
    aic_stmt_bind_text(st, 9, m->model_id);
    aic_stmt_bind_text(st, 10, m->created_at);
    aic_stmt_bind_ll(st, 11, m->superseded);
    aic_stmt_bind_ll(st, 12, m->logical_ts);
    aic_stmt_bind_text(st, 13, m->origin_device);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_transfer_import(aic_db *db, const char *path, aic_conflict_policy policy,
                        aic_import_result *out, char *err, size_t errlen)
{
    if (!db || !path) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    aic_import_result local;
    memset(&local, 0, sizeof(local));

    tr_doc doc;
    memset(&doc, 0, sizeof(doc));
    int rc = parse_doc(path, &doc, err, errlen);
    if (rc != AIC_OK) { tr_doc_free(&doc); return rc; }

    rc = aic_db_begin(db);
    if (rc != AIC_OK) { tr_doc_free(&doc); return rc; }

    for (size_t i = 0; i < doc.conv_n && rc == AIC_OK; i++)
        rc = import_conversation(db, &doc.convs[i], policy, &local);
    for (size_t i = 0; i < doc.msg_n && rc == AIC_OK; i++)
        rc = import_message(db, &doc.msgs[i], policy, &local);

    if (rc != AIC_OK) {
        aic_db_rollback(db);
        set_err(err, errlen, "导入失败已回滚：%s", aic_strerror(rc));
    } else {
        rc = aic_db_commit(db);
        if (rc != AIC_OK) set_err(err, errlen, "提交失败已回滚：%s", aic_strerror(rc));
    }
    tr_doc_free(&doc);
    if (rc == AIC_OK && out) *out = local;
    return rc;
}
