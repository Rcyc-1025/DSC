/* store_message.c */
#include "store_message.h"
#include "sync_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSG_COLS "id, conversation_id, seq, role, content, recalled, IFNULL(edited_at,''), " \
                 "original_content, IFNULL(model_id,''), created_at, superseded, " \
                 "logical_ts, origin_device"

static void read_row(dsc_stmt *st, dsc_message *m)
{
    const char *v;
    memset(m, 0, sizeof(*m));
    if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(m->id, sizeof(m->id), v);
    if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(m->conversation_id, sizeof(m->conversation_id), v);
    m->seq = dsc_stmt_ll(st, 2);
    if ((v = dsc_stmt_text(st, 3))) dsc_strlcpy_utf8(m->role, sizeof(m->role), v);
    if ((v = dsc_stmt_text(st, 4))) m->content = dsc_strdup(v);
    m->recalled = dsc_stmt_int(st, 5);
    if ((v = dsc_stmt_text(st, 6))) dsc_strlcpy_utf8(m->edited_at, sizeof(m->edited_at), v);
    if (!dsc_stmt_is_null(st, 7) && (v = dsc_stmt_text(st, 7))) m->original_content = dsc_strdup(v);
    if ((v = dsc_stmt_text(st, 8))) dsc_strlcpy_utf8(m->model_id, sizeof(m->model_id), v);
    if ((v = dsc_stmt_text(st, 9))) dsc_strlcpy_utf8(m->created_at, sizeof(m->created_at), v);
    m->superseded = dsc_stmt_int(st, 10);
    m->logical_ts = dsc_stmt_ll(st, 11);
    if ((v = dsc_stmt_text(st, 12))) dsc_strlcpy_utf8(m->origin_device, sizeof(m->origin_device), v);
}

void dsc_msg_free(dsc_message *m)
{
    if (!m) return;
    dsc_xfree(m->content);
    dsc_xfree(m->original_content);
    m->content = NULL;
    m->original_content = NULL;
}

void dsc_msg_free_array(dsc_message *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) dsc_msg_free(&arr[i]);
    dsc_xfree(arr);
}

int dsc_msg_append(dsc_db *db, const char *conv_id, dsc_role role, const char *content,
                   const char *model_id, dsc_message *out)
{
    if (!db || !conv_id || !content) return DSC_E_INVAL;
    const char *role_s = dsc_role_name(role);
    if (!role_s) return DSC_E_INVAL;
    size_t clen = strlen(content);
    if (clen > DSC_CONTENT_MAX_BYTES) return DSC_E_LIMIT;

    int rc = dsc_db_begin(db);
    if (rc != DSC_OK) return rc;

    /* 逻辑时钟与来源设备（技术设计 §16.5）：本地新增取本端最大值加 1，
     * 与 seq 一起构成跨端排序键 (logical_ts, origin_device, seq)。 */
    long long lts = 0;
    rc = dsc_clock_next(db, &lts);
    if (rc != DSC_OK) { dsc_db_rollback(db); return rc; }
    char dev[40];
    dev[0] = '\0';
    if (dsc_device_id(db, dev, sizeof(dev)) != DSC_OK) dev[0] = '\0';

    /* 会话必须存在，并在同一事务内推进 last_seq。 */
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE conversation SET last_seq = last_seq + 1, updated_at = ?2 WHERE id = ?1 RETURNING last_seq;");
    if (!st) { dsc_db_rollback(db); return DSC_E_DB; }
    char now[32];
    dsc_now_iso8601(now);
    dsc_stmt_bind_text(st, 1, conv_id);
    dsc_stmt_bind_text(st, 2, now);
    rc = dsc_stmt_step(st);
    long long seq = 0;
    if (rc == DSC_ROW) seq = dsc_stmt_ll(st, 0);
    int saw = (rc == DSC_ROW);
    dsc_stmt_finalize(st);
    if (!saw) {
        dsc_db_rollback(db);
        return rc == DSC_DONE ? DSC_E_NOTFOUND : rc;
    }

    dsc_message m;
    memset(&m, 0, sizeof(m));
    dsc_uuid4(m.id);
    dsc_strlcpy_utf8(m.conversation_id, sizeof(m.conversation_id), conv_id);
    m.seq = seq;
    dsc_strlcpy_utf8(m.role, sizeof(m.role), role_s);
    m.content = dsc_strdup(content);
    if (model_id && *model_id) dsc_strlcpy_utf8(m.model_id, sizeof(m.model_id), model_id);
    dsc_strlcpy_utf8(m.created_at, sizeof(m.created_at), now);
    m.logical_ts = lts;
    dsc_strlcpy_utf8(m.origin_device, sizeof(m.origin_device), dev);

    st = dsc_db_prepare(db,
        "INSERT INTO message (id, conversation_id, seq, role, content, recalled, edited_at, "
        "original_content, model_id, created_at, logical_ts, origin_device) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 0, NULL, NULL, ?6, ?7, ?8, ?9);");
    if (!st) { dsc_msg_free(&m); dsc_db_rollback(db); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, m.id);
    dsc_stmt_bind_text(st, 2, m.conversation_id);
    dsc_stmt_bind_ll(st, 3, m.seq);
    dsc_stmt_bind_text(st, 4, m.role);
    dsc_stmt_bind_text(st, 5, m.content);
    dsc_stmt_bind_text(st, 6, m.model_id[0] ? m.model_id : NULL);
    dsc_stmt_bind_text(st, 7, m.created_at);
    dsc_stmt_bind_ll(st, 8, m.logical_ts);
    dsc_stmt_bind_text(st, 9, m.origin_device);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) {
        dsc_msg_free(&m);
        dsc_db_rollback(db);
        return rc;
    }
    rc = dsc_db_commit(db);
    if (rc != DSC_OK) {
        dsc_msg_free(&m);
        return rc;
    }
    if (out) *out = m;
    else dsc_msg_free(&m);
    return DSC_OK;
}

int dsc_msg_get(dsc_db *db, const char *id, dsc_message *out)
{
    if (!db || !id || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "SELECT " MSG_COLS " FROM message WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_row(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

static void reverse_messages(dsc_message *arr, size_t n)
{
    for (size_t i = 0; i < n / 2; i++) {
        dsc_message t = arr[i];
        arr[i] = arr[n - 1 - i];
        arr[n - 1 - i] = t;
    }
}

static int collect_msgs(dsc_stmt *st, dsc_message **out, size_t *out_n)
{
    size_t cap = 32, n = 0;
    dsc_message *arr = (dsc_message *)dsc_xcalloc(cap, sizeof(dsc_message));
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (dsc_message *)dsc_xrealloc(arr, cap * sizeof(dsc_message));
        }
        read_row(st, &arr[n++]);
    }
    if (rc != DSC_DONE) {
        dsc_msg_free_array(arr, n);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

int dsc_msg_page(dsc_db *db, const char *conv_id, long long before_seq, int limit,
                 dsc_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return DSC_E_INVAL;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    dsc_stmt *st;
    if (before_seq > 0) {
        st = dsc_db_prepare(db,
            "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND seq < ?2 "
            "AND superseded = 0 ORDER BY logical_ts DESC, origin_device DESC, seq DESC LIMIT ?3;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, conv_id);
        dsc_stmt_bind_ll(st, 2, before_seq);
        dsc_stmt_bind_ll(st, 3, limit);
    } else {
        st = dsc_db_prepare(db,
            "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND superseded = 0 "
            "ORDER BY logical_ts DESC, origin_device DESC, seq DESC LIMIT ?2;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, conv_id);
        dsc_stmt_bind_ll(st, 2, limit);
    }
    int rc = collect_msgs(st, out, out_n);
    dsc_stmt_finalize(st);
    if (rc == DSC_OK) reverse_messages(*out, *out_n);
    return rc;
}

int dsc_msg_page_ordered(dsc_db *db, const char *conv_id,
                          long long before_ts, const char *before_device, long long before_seq,
                          int limit, dsc_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return DSC_E_INVAL;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    const char *dev = before_device ? before_device : "";
    dsc_stmt *st;
    if (before_ts > 0) {
        /* 行值比较：严格小于 (before_ts, before_device, before_seq)，跨端顺序一致。 */
        st = dsc_db_prepare(db,
            "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND superseded = 0 "
            "AND (logical_ts, origin_device, seq) < (?2, ?3, ?4) "
            "ORDER BY logical_ts DESC, origin_device DESC, seq DESC LIMIT ?5;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, conv_id);
        dsc_stmt_bind_ll(st, 2, before_ts);
        dsc_stmt_bind_text(st, 3, dev);
        dsc_stmt_bind_ll(st, 4, before_seq);
        dsc_stmt_bind_ll(st, 5, limit);
    } else {
        st = dsc_db_prepare(db,
            "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND superseded = 0 "
            "ORDER BY logical_ts DESC, origin_device DESC, seq DESC LIMIT ?2;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, conv_id);
        dsc_stmt_bind_ll(st, 2, limit);
    }
    int rc = collect_msgs(st, out, out_n);
    dsc_stmt_finalize(st);
    if (rc == DSC_OK) reverse_messages(*out, *out_n);
    return rc;
}

int dsc_msg_context(dsc_db *db, const char *conv_id, int limit,
                    dsc_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return DSC_E_INVAL;
    if (limit <= 0) limit = DSC_CONTEXT_MESSAGES_DEFAULT;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND recalled = 0 "
        "AND superseded = 0 ORDER BY logical_ts DESC, origin_device DESC, seq DESC LIMIT ?2;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, conv_id);
    dsc_stmt_bind_ll(st, 2, limit);
    int rc = collect_msgs(st, out, out_n);
    dsc_stmt_finalize(st);
    if (rc == DSC_OK) reverse_messages(*out, *out_n);
    return rc;
}

int dsc_msg_recall(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_message m;
    int rc = dsc_msg_get(db, id, &m);
    if (rc != DSC_OK) return rc;
    if (m.recalled) { dsc_msg_free(&m); return DSC_E_STATE; }
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE message SET recalled = 1, original_content = content, content = ?2, "
        "edited_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id = ?1;");
    if (!st) { dsc_msg_free(&m); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, DSC_RECALL_NOTICE);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    dsc_msg_free(&m);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_msg_unrecall(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_message m;
    int rc = dsc_msg_get(db, id, &m);
    if (rc != DSC_OK) return rc;
    if (!m.recalled || !m.original_content) { dsc_msg_free(&m); return DSC_E_STATE; }
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE message SET recalled = 0, content = original_content WHERE id = ?1;");
    if (!st) { dsc_msg_free(&m); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, id);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    dsc_msg_free(&m);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_msg_edit(dsc_db *db, const char *id, const char *new_content, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!db || !id || !new_content) return DSC_E_INVAL;
    size_t clen = strlen(new_content);
    if (clen == 0) {
        if (err && errlen) snprintf(err, errlen, "消息内容不能为空");
        return DSC_E_INVAL;
    }
    if (clen > DSC_CONTENT_MAX_BYTES) {
        if (err && errlen) snprintf(err, errlen, "消息内容超出上限");
        return DSC_E_LIMIT;
    }
    dsc_message m;
    int rc = dsc_msg_get(db, id, &m);
    if (rc != DSC_OK) return rc;
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE message SET content = ?2, "
        "original_content = CASE WHEN original_content IS NULL AND recalled = 0 THEN content ELSE original_content END, "
        "edited_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id = ?1;");
    if (!st) { dsc_msg_free(&m); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, new_content);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    dsc_msg_free(&m);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_msg_delete(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "DELETE FROM message WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_msg_supersede(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "UPDATE message SET superseded = 1 WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_msg_restore(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE message SET content = COALESCE(original_content, content), "
        "original_content = NULL, edited_at = NULL WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}
int dsc_msg_last_user(dsc_db *db, const char *conv_id, dsc_message *out)
{
    if (!db || !conv_id || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND role = 'user' "
        "AND recalled = 0 AND superseded = 0 ORDER BY seq DESC LIMIT 1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, conv_id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_row(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_msg_last_assistant(dsc_db *db, const char *conv_id, dsc_message *out)
{
    if (!db || !conv_id || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND role = 'assistant' "
        "AND recalled = 0 AND superseded = 0 ORDER BY seq DESC LIMIT 1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, conv_id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_row(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_msg_count(dsc_db *db, const char *conv_id, long long *out_count)
{
    if (!db || !conv_id || !out_count) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "SELECT COUNT(*) FROM message WHERE conversation_id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, conv_id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) *out_count = dsc_stmt_ll(st, 0);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : DSC_E_DB;
}

int64_t dsc_msg_created_ms(dsc_db *db, const char *id)
{
    if (!db || !id) return 0;
    dsc_stmt *st = dsc_db_prepare(db, "SELECT created_at FROM message WHERE id = ?1;");
    if (!st) return 0;
    dsc_stmt_bind_text(st, 1, id);
    int64_t ms = 0;
    if (dsc_stmt_step(st) == DSC_ROW) {
        const char *s = dsc_stmt_text(st, 0);
        dsc_parse_iso8601(s, &ms);
    }
    dsc_stmt_finalize(st);
    return ms;
}

int dsc_msg_export_all(dsc_db *db, const char *conv_id, dsc_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return DSC_E_INVAL;
    *out = NULL;
    *out_n = 0;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 ORDER BY seq ASC;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, conv_id);
    size_t cap = 64, n = 0;
    dsc_message *arr = dsc_xcalloc(cap, sizeof(dsc_message));
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = dsc_xrealloc(arr, cap * sizeof(dsc_message));
            memset(arr + n, 0, (cap - n) * sizeof(dsc_message));
        }
        read_row(st, &arr[n]);
        n++;
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) { dsc_msg_free_array(arr, n); return DSC_E_DB; }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

