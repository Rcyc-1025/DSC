/* store_message.c */
#include "store_message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSG_COLS "id, conversation_id, seq, role, content, recalled, IFNULL(edited_at,''), " \
                 "original_content, IFNULL(model_id,''), created_at"

static void read_row(aic_stmt *st, aic_message *m)
{
    const char *v;
    memset(m, 0, sizeof(*m));
    if ((v = aic_stmt_text(st, 0))) aic_strlcpy_utf8(m->id, sizeof(m->id), v);
    if ((v = aic_stmt_text(st, 1))) aic_strlcpy_utf8(m->conversation_id, sizeof(m->conversation_id), v);
    m->seq = aic_stmt_ll(st, 2);
    if ((v = aic_stmt_text(st, 3))) aic_strlcpy_utf8(m->role, sizeof(m->role), v);
    if ((v = aic_stmt_text(st, 4))) m->content = aic_strdup(v);
    m->recalled = aic_stmt_int(st, 5);
    if ((v = aic_stmt_text(st, 6))) aic_strlcpy_utf8(m->edited_at, sizeof(m->edited_at), v);
    if (!aic_stmt_is_null(st, 7) && (v = aic_stmt_text(st, 7))) m->original_content = aic_strdup(v);
    if ((v = aic_stmt_text(st, 8))) aic_strlcpy_utf8(m->model_id, sizeof(m->model_id), v);
    if ((v = aic_stmt_text(st, 9))) aic_strlcpy_utf8(m->created_at, sizeof(m->created_at), v);
}

void aic_msg_free(aic_message *m)
{
    if (!m) return;
    aic_xfree(m->content);
    aic_xfree(m->original_content);
    m->content = NULL;
    m->original_content = NULL;
}

void aic_msg_free_array(aic_message *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) aic_msg_free(&arr[i]);
    aic_xfree(arr);
}

int aic_msg_append(aic_db *db, const char *conv_id, aic_role role, const char *content,
                   const char *model_id, aic_message *out)
{
    if (!db || !conv_id || !content) return AIC_E_INVAL;
    const char *role_s = aic_role_name(role);
    if (!role_s) return AIC_E_INVAL;
    size_t clen = strlen(content);
    if (clen > AIC_CONTENT_MAX_BYTES) return AIC_E_LIMIT;

    int rc = aic_db_begin(db);
    if (rc != AIC_OK) return rc;

    /* 会话必须存在，并在同一事务内推进 last_seq。 */
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE conversation SET last_seq = last_seq + 1, updated_at = ?2 WHERE id = ?1 RETURNING last_seq;");
    if (!st) { aic_db_rollback(db); return AIC_E_DB; }
    char now[32];
    aic_now_iso8601(now);
    aic_stmt_bind_text(st, 1, conv_id);
    aic_stmt_bind_text(st, 2, now);
    rc = aic_stmt_step(st);
    long long seq = 0;
    if (rc == AIC_ROW) seq = aic_stmt_ll(st, 0);
    int saw = (rc == AIC_ROW);
    aic_stmt_finalize(st);
    if (!saw) {
        aic_db_rollback(db);
        return rc == AIC_DONE ? AIC_E_NOTFOUND : rc;
    }

    aic_message m;
    memset(&m, 0, sizeof(m));
    aic_uuid4(m.id);
    aic_strlcpy_utf8(m.conversation_id, sizeof(m.conversation_id), conv_id);
    m.seq = seq;
    aic_strlcpy_utf8(m.role, sizeof(m.role), role_s);
    m.content = aic_strdup(content);
    if (model_id && *model_id) aic_strlcpy_utf8(m.model_id, sizeof(m.model_id), model_id);
    aic_strlcpy_utf8(m.created_at, sizeof(m.created_at), now);

    st = aic_db_prepare(db,
        "INSERT INTO message (id, conversation_id, seq, role, content, recalled, edited_at, "
        "original_content, model_id, created_at) VALUES (?1, ?2, ?3, ?4, ?5, 0, NULL, NULL, ?6, ?7);");
    if (!st) { aic_msg_free(&m); aic_db_rollback(db); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, m.id);
    aic_stmt_bind_text(st, 2, m.conversation_id);
    aic_stmt_bind_ll(st, 3, m.seq);
    aic_stmt_bind_text(st, 4, m.role);
    aic_stmt_bind_text(st, 5, m.content);
    aic_stmt_bind_text(st, 6, m.model_id[0] ? m.model_id : NULL);
    aic_stmt_bind_text(st, 7, m.created_at);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) {
        aic_msg_free(&m);
        aic_db_rollback(db);
        return rc;
    }
    rc = aic_db_commit(db);
    if (rc != AIC_OK) {
        aic_msg_free(&m);
        return rc;
    }
    if (out) *out = m;
    else aic_msg_free(&m);
    return AIC_OK;
}

int aic_msg_get(aic_db *db, const char *id, aic_message *out)
{
    if (!db || !id || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "SELECT " MSG_COLS " FROM message WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) read_row(st, out);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : (rc == AIC_DONE ? AIC_E_NOTFOUND : rc);
}

static void reverse_messages(aic_message *arr, size_t n)
{
    for (size_t i = 0; i < n / 2; i++) {
        aic_message t = arr[i];
        arr[i] = arr[n - 1 - i];
        arr[n - 1 - i] = t;
    }
}

static int collect_msgs(aic_stmt *st, aic_message **out, size_t *out_n)
{
    size_t cap = 32, n = 0;
    aic_message *arr = (aic_message *)aic_xcalloc(cap, sizeof(aic_message));
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (aic_message *)aic_xrealloc(arr, cap * sizeof(aic_message));
        }
        read_row(st, &arr[n++]);
    }
    if (rc != AIC_DONE) {
        aic_msg_free_array(arr, n);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

int aic_msg_page(aic_db *db, const char *conv_id, long long before_seq, int limit,
                 aic_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return AIC_E_INVAL;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    aic_stmt *st;
    if (before_seq > 0) {
        st = aic_db_prepare(db,
            "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND seq < ?2 "
            "ORDER BY seq DESC LIMIT ?3;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, conv_id);
        aic_stmt_bind_ll(st, 2, before_seq);
        aic_stmt_bind_ll(st, 3, limit);
    } else {
        st = aic_db_prepare(db,
            "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 "
            "ORDER BY seq DESC LIMIT ?2;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, conv_id);
        aic_stmt_bind_ll(st, 2, limit);
    }
    int rc = collect_msgs(st, out, out_n);
    aic_stmt_finalize(st);
    if (rc == AIC_OK) reverse_messages(*out, *out_n);
    return rc;
}

int aic_msg_context(aic_db *db, const char *conv_id, int limit,
                    aic_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return AIC_E_INVAL;
    if (limit <= 0) limit = AIC_CONTEXT_MESSAGES_DEFAULT;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 AND recalled = 0 "
        "ORDER BY seq DESC LIMIT ?2;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, conv_id);
    aic_stmt_bind_ll(st, 2, limit);
    int rc = collect_msgs(st, out, out_n);
    aic_stmt_finalize(st);
    if (rc == AIC_OK) reverse_messages(*out, *out_n);
    return rc;
}

int aic_msg_recall(aic_db *db, const char *id)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_message m;
    int rc = aic_msg_get(db, id, &m);
    if (rc != AIC_OK) return rc;
    if (m.recalled) { aic_msg_free(&m); return AIC_E_STATE; }
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE message SET recalled = 1, original_content = content, content = ?2, "
        "edited_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id = ?1;");
    if (!st) { aic_msg_free(&m); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, AIC_RECALL_NOTICE);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    aic_msg_free(&m);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_msg_unrecall(aic_db *db, const char *id)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_message m;
    int rc = aic_msg_get(db, id, &m);
    if (rc != AIC_OK) return rc;
    if (!m.recalled || !m.original_content) { aic_msg_free(&m); return AIC_E_STATE; }
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE message SET recalled = 0, content = original_content WHERE id = ?1;");
    if (!st) { aic_msg_free(&m); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, id);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    aic_msg_free(&m);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_msg_edit(aic_db *db, const char *id, const char *new_content, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!db || !id || !new_content) return AIC_E_INVAL;
    size_t clen = strlen(new_content);
    if (clen == 0) {
        if (err && errlen) snprintf(err, errlen, "消息内容不能为空");
        return AIC_E_INVAL;
    }
    if (clen > AIC_CONTENT_MAX_BYTES) {
        if (err && errlen) snprintf(err, errlen, "消息内容超出上限");
        return AIC_E_LIMIT;
    }
    aic_message m;
    int rc = aic_msg_get(db, id, &m);
    if (rc != AIC_OK) return rc;
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE message SET content = ?2, "
        "original_content = CASE WHEN original_content IS NULL AND recalled = 0 THEN content ELSE original_content END, "
        "edited_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id = ?1;");
    if (!st) { aic_msg_free(&m); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, new_content);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    aic_msg_free(&m);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_msg_delete(aic_db *db, const char *id)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "DELETE FROM message WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_msg_count(aic_db *db, const char *conv_id, long long *out_count)
{
    if (!db || !conv_id || !out_count) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "SELECT COUNT(*) FROM message WHERE conversation_id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, conv_id);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) *out_count = aic_stmt_ll(st, 0);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : AIC_E_DB;
}

int64_t aic_msg_created_ms(aic_db *db, const char *id)
{
    if (!db || !id) return 0;
    aic_stmt *st = aic_db_prepare(db, "SELECT created_at FROM message WHERE id = ?1;");
    if (!st) return 0;
    aic_stmt_bind_text(st, 1, id);
    int64_t ms = 0;
    if (aic_stmt_step(st) == AIC_ROW) {
        const char *s = aic_stmt_text(st, 0);
        aic_parse_iso8601(s, &ms);
    }
    aic_stmt_finalize(st);
    return ms;
}

int aic_msg_export_all(aic_db *db, const char *conv_id, aic_message **out, size_t *out_n)
{
    if (!db || !conv_id || !out || !out_n) return AIC_E_INVAL;
    *out = NULL;
    *out_n = 0;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " MSG_COLS " FROM message WHERE conversation_id = ?1 ORDER BY seq ASC;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, conv_id);
    size_t cap = 64, n = 0;
    aic_message *arr = aic_xcalloc(cap, sizeof(aic_message));
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = aic_xrealloc(arr, cap * sizeof(aic_message));
            memset(arr + n, 0, (cap - n) * sizeof(aic_message));
        }
        read_row(st, &arr[n]);
        n++;
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) { aic_msg_free_array(arr, n); return AIC_E_DB; }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

