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
    char id[DSC_ID_BYTES];
    char mode[8];
    char title[DSC_TITLE_BYTES];
    char persona_id[DSC_ID_BYTES];
    char model_id[DSC_MODEL_BYTES];
    int  pinned;
    long long last_seq;
    char created_at[DSC_TS_BYTES];
    char updated_at[DSC_TS_BYTES];
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
        dsc_xfree(d->msgs[i].content);
        dsc_xfree(d->msgs[i].original_content);
    }
    dsc_xfree(d->msgs);
    dsc_xfree(d->convs);
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
static void checksum_feed(dsc_sha256_ctx *c, const tr_msg *m)
{
    const unsigned char sep = 0x1F;
    dsc_sha256_update(c, m->id, strlen(m->id));
    dsc_sha256_update(c, &sep, 1);
    const char *body = m->content ? m->content : "";
    dsc_sha256_update(c, body, strlen(body));
    dsc_sha256_update(c, &sep, 1);
}

static void checksum_of(const tr_msg *msgs, size_t n, char out[65])
{
    dsc_sha256_ctx c;
    dsc_sha256_init(&c);
    for (size_t i = 0; i < n; i++) checksum_feed(&c, &msgs[i]);
    unsigned char digest[32];
    dsc_sha256_final(&c, digest);
    dsc_hex_encode(digest, 32, out);
}

/* ---------- 导出 ---------- */
/* scope=2 的时间区间转成 ISO 文本，与 message.created_at 做字典序比较（同为 UTC 定长格式）。 */
static void scope_bounds(const dsc_export_scope *sc, char *from_iso, char *to_iso)
{
    from_iso[0] = '\0';
    to_iso[0] = '\0';
    if (!sc || sc->scope != 2) return;
    if (sc->from_ms > 0) dsc_ms_to_iso8601(sc->from_ms, from_iso);
    if (sc->to_ms > 0) dsc_ms_to_iso8601(sc->to_ms, to_iso);
}

static int collect(dsc_db *db, int mode, const dsc_export_scope *sc, tr_doc *doc,
                   char *err, size_t errlen)
{
    dsc_conversation *convs = NULL;
    size_t conv_n = 0;
    int rc = dsc_conv_list(db, mode, &convs, &conv_n);
    if (rc != DSC_OK) { set_err(err, errlen, "读取会话失败：%s", dsc_strerror(rc)); return rc; }

    doc->convs = dsc_xcalloc(conv_n ? conv_n : 1, sizeof(tr_conv));
    doc->conv_n = 0;
    size_t msg_cap = 64, msg_n = 0;
    doc->msgs = dsc_xcalloc(msg_cap, sizeof(tr_msg));

    char from_iso[32], to_iso[32];
    scope_bounds(sc, from_iso, to_iso);

    for (size_t i = 0; i < conv_n; i++) {
        if (sc && sc->scope == 1) {
            if (!sc->conv_id || strcmp(convs[i].id, sc->conv_id) != 0) continue;
        }
        tr_conv *tc = &doc->convs[doc->conv_n++];
        dsc_strlcpy_utf8(tc->id, sizeof(tc->id), convs[i].id);
        dsc_strlcpy_utf8(tc->mode, sizeof(tc->mode), convs[i].mode);
        dsc_strlcpy_utf8(tc->title, sizeof(tc->title), convs[i].title);
        dsc_strlcpy_utf8(tc->persona_id, sizeof(tc->persona_id), convs[i].persona_id);
        dsc_strlcpy_utf8(tc->model_id, sizeof(tc->model_id), convs[i].model_id);
        tc->pinned = convs[i].pinned;
        tc->last_seq = convs[i].last_seq;
        dsc_strlcpy_utf8(tc->created_at, sizeof(tc->created_at), convs[i].created_at);
        dsc_strlcpy_utf8(tc->updated_at, sizeof(tc->updated_at), convs[i].updated_at);

        dsc_message *msgs = NULL;
        size_t n = 0;
        rc = dsc_msg_export_all(db, convs[i].id, &msgs, &n);
        if (rc != DSC_OK) {
            dsc_conv_free(convs, conv_n);
            set_err(err, errlen, "读取消息失败：%s", dsc_strerror(rc));
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
                doc->msgs = dsc_xrealloc(doc->msgs, msg_cap * sizeof(tr_msg));
                memset(doc->msgs + msg_n, 0, (msg_cap - msg_n) * sizeof(tr_msg));
            }
            tr_msg *tm = &doc->msgs[msg_n++];
            dsc_strlcpy_utf8(tm->id, sizeof(tm->id), msgs[k].id);
            dsc_strlcpy_utf8(tm->conversation_id, sizeof(tm->conversation_id), msgs[k].conversation_id);
            tm->seq = msgs[k].seq;
            dsc_strlcpy_utf8(tm->role, sizeof(tm->role), msgs[k].role);
            tm->content = dsc_strdup(msgs[k].content ? msgs[k].content : "");
            tm->recalled = msgs[k].recalled;
            tm->superseded = msgs[k].superseded;
            dsc_strlcpy_utf8(tm->edited_at, sizeof(tm->edited_at), msgs[k].edited_at);
            tm->original_content = msgs[k].original_content ? dsc_strdup(msgs[k].original_content) : NULL;
            dsc_strlcpy_utf8(tm->model_id, sizeof(tm->model_id), msgs[k].model_id);
            dsc_strlcpy_utf8(tm->created_at, sizeof(tm->created_at), msgs[k].created_at);
            tm->logical_ts = msgs[k].logical_ts;
            dsc_strlcpy_utf8(tm->origin_device, sizeof(tm->origin_device), msgs[k].origin_device);
        }
        dsc_msg_free_array(msgs, n);
        if (sc && sc->scope == 2 && msg_n == kept_before) doc->conv_n--;  /* 区间内没有消息的会话不导出 */
    }
    doc->msg_n = msg_n;
    dsc_conv_free(convs, conv_n);
    return DSC_OK;
}

static void emit_avatars(dsc_db *db, jw *w, const dsc_export_scope *sc)
{
    jw_arr_begin(w, "avatars");
    if (!sc || !sc->include_avatars || !sc->root || !sc->root[0]) { jw_arr_end(w); return; }
    dsc_profile p;
    memset(&p, 0, sizeof(p));
    if (dsc_profile_get(db, &p) != DSC_OK || !p.avatar_path[0]) { jw_arr_end(w); return; }
    char full[1024];
    if (dsc_path_join(full, sizeof(full), sc->root, p.avatar_path) != DSC_OK) { jw_arr_end(w); return; }
    char *data = NULL;
    size_t len = 0;
    if (dsc_file_read_all(full, &data, &len) != DSC_OK) { jw_arr_end(w); return; }
    char sha[65];
    dsc_sha256_hex(data, len, sha);
    sb b64;
    sb_init(&b64);
    dsc_base64_encode(data, len, &b64);
    dsc_xfree(data);
    jw_obj_begin(w, NULL);
    jw_put_str(w, "path", p.avatar_path);
    jw_put_str(w, "sha256", sha);
    jw_put_str(w, "data_base64", sb_cstr(&b64));
    jw_obj_end(w);
    sb_free(&b64);
    jw_arr_end(w);
}

int dsc_export_json(dsc_db *db, int mode, sb *out, char *err, size_t errlen)
{
    return dsc_export_json_ex(db, mode, NULL, out, err, errlen);
}

int dsc_export_json_ex(dsc_db *db, int mode, const dsc_export_scope *sc, sb *out,
                       char *err, size_t errlen)
{
    if (!db || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    tr_doc doc;
    memset(&doc, 0, sizeof(doc));
    doc.schema_version = DSC_EXPORT_SCHEMA_VER;
    dsc_now_iso8601(doc.exported_at);
    if (mode == DSC_MODE_QA) dsc_strlcpy_utf8(doc.source_mode, sizeof(doc.source_mode), "qa");
    else if (mode == DSC_MODE_CHAT) dsc_strlcpy_utf8(doc.source_mode, sizeof(doc.source_mode), "chat");
    else dsc_strlcpy_utf8(doc.source_mode, sizeof(doc.source_mode), "all");

    int rc = collect(db, mode, sc, &doc, err, errlen);
    if (rc != DSC_OK) { tr_doc_free(&doc); return rc; }
    checksum_of(doc.msgs, doc.msg_n, doc.checksum);

    jw w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "format", DSC_EXPORT_FORMAT);
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
    return DSC_OK;
}

static int write_file(const char *path, sb *content, char *err, size_t errlen)
{
    int rc = dsc_file_write_all(path, sb_cstr(content), content->len);
    if (rc != DSC_OK) set_err(err, errlen, "写入文件失败：%s", dsc_strerror(rc));
    return rc;
}

int dsc_export_write_json_ex(dsc_db *db, int mode, const dsc_export_scope *sc, const char *path,
                             char *err, size_t errlen)
{
    if (!path) return DSC_E_INVAL;
    sb b;
    sb_init(&b);
    int rc = dsc_export_json_ex(db, mode, sc, &b, err, errlen);
    if (rc == DSC_OK) rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

int dsc_export_write_json(dsc_db *db, int mode, const char *path, char *err, size_t errlen)
{
    if (!path) return DSC_E_INVAL;
    sb b;
    sb_init(&b);
    int rc = dsc_export_json(db, mode, &b, err, errlen);
    if (rc == DSC_OK) rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

static void write_conv_body(dsc_db *db, int mode, const dsc_export_scope *sc, sb *out,
                            int markdown, char *err, size_t errlen)
{
    dsc_conversation *convs = NULL;
    size_t n = 0;
    char from_iso[32], to_iso[32];
    scope_bounds(sc, from_iso, to_iso);
    if (dsc_conv_list(db, mode, &convs, &n) != DSC_OK) {
        set_err(err, errlen, "读取会话失败");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const dsc_conversation *c = &convs[i];
        size_t written = 0;
        if (sc && sc->scope == 1) {
            if (!sc->conv_id || strcmp(c->id, sc->conv_id) != 0) continue;
        }
        if (markdown) sb_appendf(out, "# %s（%s）\n\n", c->title, c->mode);
        else sb_appendf(out, "===== %s（%s） =====\n\n", c->title, c->mode);
        dsc_message *msgs = NULL;
        size_t mn = 0;
        if (dsc_msg_export_all(db, c->id, &msgs, &mn) == DSC_OK) {
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
                    sb_appendf(out, "[%s] %s\n\n", who, DSC_RECALL_NOTICE);
                } else if (markdown) {
                    sb_appendf(out, "**%s**：%s\n\n", who, msgs[k].content ? msgs[k].content : "");
                } else {
                    sb_appendf(out, "%s：%s\n\n", who, msgs[k].content ? msgs[k].content : "");
                }
            }
            dsc_msg_free_array(msgs, mn);
        }
        if (sc && sc->scope == 2 && written == 0) continue;   /* 区间内没有消息的会话不导出 */
        if (markdown) sb_append(out, "---\n\n");
    }
    dsc_conv_free(convs, n);
}

int dsc_export_write_markdown_ex(dsc_db *db, int mode, const dsc_export_scope *sc, const char *path,
                                 char *err, size_t errlen)
{
    if (!db || !path) return DSC_E_INVAL;
    sb b;
    sb_init(&b);
    sb_append(&b, "# DSC 聊天记录导出\n\n");
    char ts[32];
    dsc_now_iso8601(ts);
    sb_appendf(&b, "导出时间：%s\n\n", ts);
    write_conv_body(db, mode, sc, &b, 1, err, errlen);
    int rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

int dsc_export_write_markdown(dsc_db *db, int mode, const char *path, char *err, size_t errlen)
{
    return dsc_export_write_markdown_ex(db, mode, NULL, path, err, errlen);
}

int dsc_export_write_text_ex(dsc_db *db, int mode, const dsc_export_scope *sc, const char *path,
                             char *err, size_t errlen)
{
    if (!db || !path) return DSC_E_INVAL;
    sb b;
    sb_init(&b);
    sb_append(&b, "DSC 聊天记录导出\n\n");
    write_conv_body(db, mode, sc, &b, 0, err, errlen);
    int rc = write_file(path, &b, err, errlen);
    sb_free(&b);
    return rc;
}

int dsc_export_write_text(dsc_db *db, int mode, const char *path, char *err, size_t errlen)
{
    return dsc_export_write_text_ex(db, mode, NULL, path, err, errlen);
}

/* ---------- 读取并校验 ---------- */
static int parse_doc(const char *path, tr_doc *doc, char *err, size_t errlen)
{
    char *text = NULL;
    size_t len = 0;
    int rc = dsc_file_read_all(path, &text, &len);
    if (rc != DSC_OK) {
        set_err(err, errlen, "无法读取导入文件：%s", dsc_strerror(rc));
        return rc;
    }
    jv *root = NULL;
    rc = jv_parse(text, len, &root, err, errlen);
    dsc_xfree(text);
    if (rc != DSC_OK) return rc;

    const char *fmt = jv_get_str(root, "format", "");
    if (strcmp(fmt, DSC_EXPORT_FORMAT) != 0) {
        set_err(err, errlen, "不是聊天记录导出文件（format=%s）", fmt);
        jv_free(root);
        return DSC_E_FORMAT;
    }
    int ver = jv_get_int(root, "schema_version", -1);
    if (ver != DSC_EXPORT_SCHEMA_VER) {
        set_err(err, errlen, "导出文件版本 %d 与当前支持的 %d 不一致", ver, DSC_EXPORT_SCHEMA_VER);
        jv_free(root);
        return DSC_E_VERSION;
    }
    doc->schema_version = ver;
    dsc_strlcpy_utf8(doc->exported_at, sizeof(doc->exported_at), jv_get_str(root, "exported_at", ""));
    dsc_strlcpy_utf8(doc->source_mode, sizeof(doc->source_mode), jv_get_str(root, "source_mode", "all"));
    dsc_strlcpy_utf8(doc->checksum, sizeof(doc->checksum), jv_get_str(root, "checksum", ""));

    const jv *carr = jv_get(root, "conversations");
    const jv *marr = jv_get(root, "messages");
    if (!carr || carr->type != JV_ARR || !marr || marr->type != JV_ARR) {
        set_err(err, errlen, "导出文件缺少 conversations 或 messages 数组");
        jv_free(root);
        return DSC_E_FORMAT;
    }

    size_t cn = jv_len(carr);
    doc->convs = dsc_xcalloc(cn ? cn : 1, sizeof(tr_conv));
    doc->conv_n = 0;
    for (size_t i = 0; i < cn; i++) {
        const jv *o = jv_at(carr, i);
        if (!o || o->type != JV_OBJ) continue;
        tr_conv *c = &doc->convs[doc->conv_n++];
        dsc_strlcpy_utf8(c->id, sizeof(c->id), jv_get_str(o, "id", ""));
        dsc_strlcpy_utf8(c->mode, sizeof(c->mode), jv_get_str(o, "mode", "qa"));
        dsc_strlcpy_utf8(c->title, sizeof(c->title), jv_get_str(o, "title", ""));
        dsc_strlcpy_utf8(c->persona_id, sizeof(c->persona_id), jv_get_str(o, "persona_id", ""));
        dsc_strlcpy_utf8(c->model_id, sizeof(c->model_id), jv_get_str(o, "model_id", DSC_MODEL_FLASH));
        c->pinned = jv_get_int(o, "pinned", 0);
        c->last_seq = jv_get_ll(o, "last_seq", 0);
        dsc_strlcpy_utf8(c->created_at, sizeof(c->created_at), jv_get_str(o, "created_at", ""));
        dsc_strlcpy_utf8(c->updated_at, sizeof(c->updated_at), jv_get_str(o, "updated_at", ""));
    }

    size_t mn = jv_len(marr);
    doc->msgs = dsc_xcalloc(mn ? mn : 1, sizeof(tr_msg));
    doc->msg_n = 0;
    for (size_t i = 0; i < mn; i++) {
        const jv *o = jv_at(marr, i);
        if (!o || o->type != JV_OBJ) continue;
        tr_msg *m = &doc->msgs[doc->msg_n++];
        dsc_strlcpy_utf8(m->id, sizeof(m->id), jv_get_str(o, "id", ""));
        dsc_strlcpy_utf8(m->conversation_id, sizeof(m->conversation_id), jv_get_str(o, "conversation_id", ""));
        m->seq = jv_get_ll(o, "seq", 0);
        dsc_strlcpy_utf8(m->role, sizeof(m->role), jv_get_str(o, "role", "user"));
        m->content = dsc_strdup(jv_get_str(o, "content", ""));
        m->recalled = jv_get_int(o, "recalled", 0);
        m->superseded = jv_get_int(o, "superseded", 0);
        dsc_strlcpy_utf8(m->edited_at, sizeof(m->edited_at), jv_get_str(o, "edited_at", ""));
        const jv *orig = jv_get(o, "original_content");
        m->original_content = (orig && orig->type == JV_STR) ? dsc_strdup(jv_string(orig)) : NULL;
        dsc_strlcpy_utf8(m->model_id, sizeof(m->model_id), jv_get_str(o, "model_id", ""));
        dsc_strlcpy_utf8(m->created_at, sizeof(m->created_at), jv_get_str(o, "created_at", ""));
        m->logical_ts = jv_get_ll(o, "logical_ts", 0);
        dsc_strlcpy_utf8(m->origin_device, sizeof(m->origin_device), jv_get_str(o, "origin_device", ""));
    }
    jv_free(root);

    /* 校验和：以解析后的消息为准重算。 */
    char calc[65];
    checksum_of(doc->msgs, doc->msg_n, calc);
    if (doc->checksum[0] && strcmp(calc, doc->checksum) != 0) {
        set_err(err, errlen, "校验和不匹配：文件可能被改动或损坏");
        tr_doc_free(doc);
        return DSC_E_CHECKSUM;
    }
    return DSC_OK;
}

static int count_conflicts(dsc_db *db, const tr_doc *doc, dsc_import_preview *out)
{
    size_t cc = 0, mc = 0;
    for (size_t i = 0; i < doc->conv_n; i++) {
        dsc_conversation c;
        if (dsc_conv_get(db, doc->convs[i].id, &c) == DSC_OK) cc++;
    }
    for (size_t i = 0; i < doc->msg_n; i++) {
        dsc_message m;
        if (dsc_msg_get(db, doc->msgs[i].id, &m) == DSC_OK) { mc++; dsc_msg_free(&m); }
    }
    out->conflict_conversations = cc;
    out->conflict_messages = mc;
    return DSC_OK;
}

int dsc_transfer_preview(dsc_db *db, const char *path, dsc_import_preview *out,
                         char *err, size_t errlen)
{
    if (!db || !path || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));
    tr_doc doc;
    memset(&doc, 0, sizeof(doc));
    int rc = parse_doc(path, &doc, err, errlen);
    if (rc != DSC_OK) { tr_doc_free(&doc); return rc; }
    out->schema_version = doc.schema_version;
    dsc_strlcpy_utf8(out->exported_at, sizeof(out->exported_at), doc.exported_at);
    dsc_strlcpy_utf8(out->source_mode, sizeof(out->source_mode), doc.source_mode);
    out->conversations = doc.conv_n;
    out->messages = doc.msg_n;
    count_conflicts(db, &doc, out);
    tr_doc_free(&doc);
    return DSC_OK;
}

/* ---------- 导入 ---------- */
static int import_conversation(dsc_db *db, const tr_conv *c, dsc_conflict_policy policy,
                               dsc_import_result *res)
{
    dsc_conversation exists;
    int found = (dsc_conv_get(db, c->id, &exists) == DSC_OK);
    if (found && policy == DSC_CONFLICT_SKIP) { res->conv_skipped++; return DSC_OK; }

    char id[40];
    dsc_strlcpy_utf8(id, sizeof(id), c->id);
    if (found && policy == DSC_CONFLICT_KEEP_BOTH) {
        dsc_uuid4(id);
        found = 0;
        res->conv_renamed++;
    }
    const char *mode = strcmp(c->mode, "chat") == 0 ? "chat" : "qa";

    dsc_stmt *st;
    if (found) {
        res->conv_overwritten++;
        st = dsc_db_prepare(db,
            "UPDATE conversation SET title=?2, persona_id=?3, model_id=?4, pinned=?5, "
            "last_seq=?6, created_at=?7, updated_at=?8 WHERE id=?1;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, id);
        dsc_stmt_bind_text(st, 2, c->title);
        dsc_stmt_bind_text(st, 3, c->persona_id);
        dsc_stmt_bind_text(st, 4, c->model_id);
        dsc_stmt_bind_ll(st, 5, c->pinned);
        dsc_stmt_bind_ll(st, 6, c->last_seq);
        dsc_stmt_bind_text(st, 7, c->created_at);
        dsc_stmt_bind_text(st, 8, c->updated_at);
    } else {
        res->conv_added++;
        st = dsc_db_prepare(db,
            "INSERT INTO conversation (id, mode, title, persona_id, model_id, pinned, last_seq, created_at, updated_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, id);
        dsc_stmt_bind_text(st, 2, mode);
        dsc_stmt_bind_text(st, 3, c->title);
        dsc_stmt_bind_text(st, 4, c->persona_id);
        dsc_stmt_bind_text(st, 5, c->model_id);
        dsc_stmt_bind_ll(st, 6, c->pinned);
        dsc_stmt_bind_ll(st, 7, c->last_seq);
        dsc_stmt_bind_text(st, 8, c->created_at);
        dsc_stmt_bind_text(st, 9, c->updated_at);
    }
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

static int seq_taken(dsc_db *db, const char *conv_id, long long seq)
{
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT 1 FROM message WHERE conversation_id=?1 AND seq=?2;");
    if (!st) return 0;
    dsc_stmt_bind_text(st, 1, conv_id);
    dsc_stmt_bind_ll(st, 2, seq);
    int taken = (dsc_stmt_step(st) == DSC_ROW);
    dsc_stmt_finalize(st);
    return taken;
}

static long long next_seq(dsc_db *db, const char *conv_id)
{
    dsc_stmt *st = dsc_db_prepare(db, "SELECT IFNULL(MAX(seq),0)+1 FROM message WHERE conversation_id=?1;");
    if (!st) return 1;
    dsc_stmt_bind_text(st, 1, conv_id);
    long long v = 1;
    if (dsc_stmt_step(st) == DSC_ROW) v = dsc_stmt_ll(st, 0);
    dsc_stmt_finalize(st);
    return v;
}

static int import_message(dsc_db *db, const tr_msg *m, dsc_conflict_policy policy,
                          dsc_import_result *res)
{
    dsc_message exists;
    int found = (dsc_msg_get(db, m->id, &exists) == DSC_OK);
    if (found) dsc_msg_free(&exists);
    if (found && policy == DSC_CONFLICT_SKIP) { res->msg_skipped++; return DSC_OK; }

    char id[40];
    dsc_strlcpy_utf8(id, sizeof(id), m->id);
    if (found && policy == DSC_CONFLICT_KEEP_BOTH) {
        dsc_uuid4(id);
        found = 0;
        res->msg_renamed++;
    }

    long long seq = m->seq;
    if (found && policy == DSC_CONFLICT_OVERWRITE) {
        res->msg_overwritten++;
        dsc_stmt *st = dsc_db_prepare(db,
            "UPDATE message SET role=?2, content=?3, recalled=?4, edited_at=?5, original_content=?6, "
            "model_id=?7, created_at=?8, superseded=?9, logical_ts=?10, origin_device=?11 WHERE id=?1;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, id);
        dsc_stmt_bind_text(st, 2, m->role);
        dsc_stmt_bind_text(st, 3, m->content ? m->content : "");
        dsc_stmt_bind_ll(st, 4, m->recalled);
        dsc_stmt_bind_text(st, 5, m->edited_at);
        if (m->original_content) dsc_stmt_bind_text(st, 6, m->original_content);
        else dsc_stmt_bind_null(st, 6);
        dsc_stmt_bind_text(st, 7, m->model_id);
        dsc_stmt_bind_text(st, 8, m->created_at);
        dsc_stmt_bind_ll(st, 9, m->superseded);
        dsc_stmt_bind_ll(st, 10, m->logical_ts);
        dsc_stmt_bind_text(st, 11, m->origin_device);
        int rc = dsc_stmt_step(st);
        dsc_stmt_finalize(st);
        return rc == DSC_DONE ? DSC_OK : rc;
    }

    /* 新增：seq 被占用时改为追加到末尾，避免唯一索引冲突。 */
    if (seq <= 0 || seq_taken(db, m->conversation_id, seq)) seq = next_seq(db, m->conversation_id);
    res->msg_added++;
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO message (id, conversation_id, seq, role, content, recalled, edited_at, "
        "original_content, model_id, created_at, superseded, logical_ts, origin_device) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, m->conversation_id);
    dsc_stmt_bind_ll(st, 3, seq);
    dsc_stmt_bind_text(st, 4, m->role);
    dsc_stmt_bind_text(st, 5, m->content ? m->content : "");
    dsc_stmt_bind_ll(st, 6, m->recalled);
    dsc_stmt_bind_text(st, 7, m->edited_at);
    if (m->original_content) dsc_stmt_bind_text(st, 8, m->original_content);
    else dsc_stmt_bind_null(st, 8);
    dsc_stmt_bind_text(st, 9, m->model_id);
    dsc_stmt_bind_text(st, 10, m->created_at);
    dsc_stmt_bind_ll(st, 11, m->superseded);
    dsc_stmt_bind_ll(st, 12, m->logical_ts);
    dsc_stmt_bind_text(st, 13, m->origin_device);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_transfer_import(dsc_db *db, const char *path, dsc_conflict_policy policy,
                        dsc_import_result *out, char *err, size_t errlen)
{
    if (!db || !path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    dsc_import_result local;
    memset(&local, 0, sizeof(local));

    tr_doc doc;
    memset(&doc, 0, sizeof(doc));
    int rc = parse_doc(path, &doc, err, errlen);
    if (rc != DSC_OK) { tr_doc_free(&doc); return rc; }

    rc = dsc_db_begin(db);
    if (rc != DSC_OK) { tr_doc_free(&doc); return rc; }

    for (size_t i = 0; i < doc.conv_n && rc == DSC_OK; i++)
        rc = import_conversation(db, &doc.convs[i], policy, &local);
    for (size_t i = 0; i < doc.msg_n && rc == DSC_OK; i++)
        rc = import_message(db, &doc.msgs[i], policy, &local);

    if (rc != DSC_OK) {
        dsc_db_rollback(db);
        set_err(err, errlen, "导入失败已回滚：%s", dsc_strerror(rc));
    } else {
        rc = dsc_db_commit(db);
        if (rc != DSC_OK) set_err(err, errlen, "提交失败已回滚：%s", dsc_strerror(rc));
    }
    tr_doc_free(&doc);
    if (rc == DSC_OK && out) *out = local;
    return rc;
}
