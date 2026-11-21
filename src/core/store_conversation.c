/* store_conversation.c */
#include "store_conversation.h"

#include <string.h>

static void read_row(aic_stmt *st, aic_conversation *c)
{
    const char *v;
    memset(c, 0, sizeof(*c));
    if ((v = aic_stmt_text(st, 0))) aic_strlcpy_utf8(c->id, sizeof(c->id), v);
    if ((v = aic_stmt_text(st, 1))) aic_strlcpy_utf8(c->mode, sizeof(c->mode), v);
    if ((v = aic_stmt_text(st, 2))) aic_strlcpy_utf8(c->title, sizeof(c->title), v);
    if ((v = aic_stmt_text(st, 3))) aic_strlcpy_utf8(c->persona_id, sizeof(c->persona_id), v);
    if ((v = aic_stmt_text(st, 4))) aic_strlcpy_utf8(c->model_id, sizeof(c->model_id), v);
    c->pinned   = aic_stmt_int(st, 5);
    c->last_seq = aic_stmt_ll(st, 6);
    if ((v = aic_stmt_text(st, 7))) aic_strlcpy_utf8(c->created_at, sizeof(c->created_at), v);
    if ((v = aic_stmt_text(st, 8))) aic_strlcpy_utf8(c->updated_at, sizeof(c->updated_at), v);
}

#define CONV_COLS "id, mode, title, IFNULL(persona_id,''), model_id, pinned, last_seq, created_at, updated_at"

int aic_conv_create(aic_db *db, aic_mode mode, const char *title, const char *persona_id,
                    const char *model_id, aic_conversation *out)
{
    if (!db || !model_id || !*model_id) return AIC_E_INVAL;
    const char *mode_s = aic_mode_name(mode);
    if (!mode_s) return AIC_E_INVAL;
    if (mode == AIC_MODE_CHAT) {
        aic_conversation exist;
        if (aic_conv_get_chat(db, &exist) == AIC_OK) return AIC_E_CONFLICT;
    }
    aic_conversation c;
    memset(&c, 0, sizeof(c));
    aic_uuid4(c.id);
    aic_strlcpy_utf8(c.mode, sizeof(c.mode), mode_s);
    aic_strlcpy_utf8(c.model_id, sizeof(c.model_id), model_id);
    if (title && *title) aic_strlcpy_utf8(c.title, sizeof(c.title), title);
    else if (mode == AIC_MODE_CHAT) aic_strlcpy_utf8(c.title, sizeof(c.title), "聊天");
    else aic_strlcpy_utf8(c.title, sizeof(c.title), "新对话");
    if (persona_id) aic_strlcpy_utf8(c.persona_id, sizeof(c.persona_id), persona_id);
    aic_now_iso8601(c.created_at);
    aic_strlcpy_utf8(c.updated_at, sizeof(c.updated_at), c.created_at);

    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO conversation (id, mode, title, persona_id, model_id, pinned, last_seq, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 0, 0, ?6, ?6);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, c.id);
    aic_stmt_bind_text(st, 2, c.mode);
    aic_stmt_bind_text(st, 3, c.title);
    aic_stmt_bind_text(st, 4, c.persona_id[0] ? c.persona_id : NULL);
    aic_stmt_bind_text(st, 5, c.model_id);
    aic_stmt_bind_text(st, 6, c.created_at);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return rc == AIC_E_CONFLICT ? AIC_E_CONFLICT : rc;
    if (out) *out = c;
    return AIC_OK;
}

int aic_conv_get(aic_db *db, const char *id, aic_conversation *out)
{
    if (!db || !id || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " CONV_COLS " FROM conversation WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) read_row(st, out);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : (rc == AIC_DONE ? AIC_E_NOTFOUND : rc);
}

int aic_conv_get_chat(aic_db *db, aic_conversation *out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " CONV_COLS " FROM conversation WHERE mode = 'chat' ORDER BY created_at ASC LIMIT 1;");
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) read_row(st, out);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : (rc == AIC_DONE ? AIC_E_NOTFOUND : rc);
}

int aic_conv_ensure_chat(aic_db *db, const char *model_id, const char *persona_id,
                         aic_conversation *out)
{
    int rc = aic_conv_get_chat(db, out);
    if (rc == AIC_OK) {
        /* 已有会话：补齐人设/模型缺省值，不改动用户已选值。 */
        if ((!out->persona_id[0] && persona_id && *persona_id) ||
            (!out->model_id[0] && model_id && *model_id)) {
            const char *p = out->persona_id[0] ? out->persona_id : persona_id;
            const char *m = out->model_id[0] ? out->model_id : model_id;
            aic_conv_update_meta(db, out->id, out->title, m, p);
            if (!out->persona_id[0] && p) aic_strlcpy_utf8(out->persona_id, sizeof(out->persona_id), p);
            if (!out->model_id[0] && m) aic_strlcpy_utf8(out->model_id, sizeof(out->model_id), m);
        }
        return AIC_OK;
    }
    if (rc != AIC_E_NOTFOUND) return rc;
    return aic_conv_create(db, AIC_MODE_CHAT, "聊天", persona_id,
                           model_id && *model_id ? model_id : AIC_MODEL_FLASH, out);
}

int aic_conv_update_meta(aic_db *db, const char *id, const char *title,
                         const char *model_id, const char *persona_id)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_conversation cur;
    int rc = aic_conv_get(db, id, &cur);
    if (rc != AIC_OK) return rc;
    const char *t = (title && *title) ? title : cur.title;
    const char *m = (model_id && *model_id) ? model_id : cur.model_id;
    const char *p = (persona_id && *persona_id) ? persona_id : cur.persona_id;
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE conversation SET title = ?2, model_id = ?3, persona_id = ?4 WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, t);
    aic_stmt_bind_text(st, 3, m);
    aic_stmt_bind_text(st, 4, p && *p ? p : NULL);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_conv_set_pinned(aic_db *db, const char *id, int pinned)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "UPDATE conversation SET pinned = ?2 WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_ll(st, 2, pinned ? 1 : 0);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_conv_touch(aic_db *db, const char *id)
{
    if (!db || !id) return AIC_E_INVAL;
    char now[AIC_TS_BYTES];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db, "UPDATE conversation SET updated_at = ?2 WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, now);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_conv_delete(aic_db *db, const char *id)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_conversation c;
    int rc = aic_conv_get(db, id, &c);
    if (rc != AIC_OK) return rc;
    if (strcmp(c.mode, "chat") == 0) return AIC_E_STATE; /* 聊天模式唯一会话不可删除 */
    aic_stmt *st = aic_db_prepare(db, "DELETE FROM conversation WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int collect(aic_db *db, aic_stmt *st, aic_conversation **out, size_t *out_n)
{
    size_t cap = 8, n = 0;
    aic_conversation *arr = (aic_conversation *)aic_xcalloc(cap, sizeof(aic_conversation));
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (aic_conversation *)aic_xrealloc(arr, cap * sizeof(aic_conversation));
        }
        read_row(st, &arr[n++]);
    }
    (void)db;
    if (rc != AIC_DONE) {
        aic_xfree(arr);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

int aic_conv_list(aic_db *db, int mode, aic_conversation **out, size_t *out_n)
{
    if (!db || !out || !out_n) return AIC_E_INVAL;
    const char *sql_all =
        "SELECT " CONV_COLS " FROM conversation ORDER BY pinned DESC, updated_at DESC;";
    const char *sql_mode =
        "SELECT " CONV_COLS " FROM conversation WHERE mode = ?1 ORDER BY pinned DESC, updated_at DESC;";
    aic_stmt *st = aic_db_prepare(db, mode < 0 ? sql_all : sql_mode);
    if (!st) return AIC_E_DB;
    if (mode >= 0) aic_stmt_bind_text(st, 1, aic_mode_name((aic_mode)mode));
    int rc = collect(db, st, out, out_n);
    aic_stmt_finalize(st);
    return rc;
}

int aic_conv_search(aic_db *db, int mode, const char *keyword, aic_conversation **out, size_t *out_n)
{
    if (!db || !keyword || !out || !out_n) return AIC_E_INVAL;
    sb pat;
    sb_init(&pat);
    sb_appendc(&pat, '%');
    sb_append(&pat, keyword);
    sb_appendc(&pat, '%');
    const char *sql_all =
        "SELECT " CONV_COLS " FROM conversation WHERE title LIKE ?1 ORDER BY pinned DESC, updated_at DESC;";
    const char *sql_mode =
        "SELECT " CONV_COLS " FROM conversation WHERE mode = ?2 AND title LIKE ?1 ORDER BY pinned DESC, updated_at DESC;";
    aic_stmt *st = aic_db_prepare(db, mode < 0 ? sql_all : sql_mode);
    if (!st) { sb_free(&pat); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, sb_cstr(&pat));
    if (mode >= 0) aic_stmt_bind_text(st, 2, aic_mode_name((aic_mode)mode));
    int rc = collect(db, st, out, out_n);
    aic_stmt_finalize(st);
    sb_free(&pat);
    return rc;
}

void aic_conv_free(aic_conversation *arr, size_t n)
{
    (void)n;
    aic_xfree(arr);
}
