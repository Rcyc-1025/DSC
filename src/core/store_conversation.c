/* store_conversation.c */
#include "store_conversation.h"

#include <string.h>

static void read_row(dsc_stmt *st, dsc_conversation *c)
{
    const char *v;
    memset(c, 0, sizeof(*c));
    if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(c->id, sizeof(c->id), v);
    if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(c->mode, sizeof(c->mode), v);
    if ((v = dsc_stmt_text(st, 2))) dsc_strlcpy_utf8(c->title, sizeof(c->title), v);
    if ((v = dsc_stmt_text(st, 3))) dsc_strlcpy_utf8(c->persona_id, sizeof(c->persona_id), v);
    if ((v = dsc_stmt_text(st, 4))) dsc_strlcpy_utf8(c->model_id, sizeof(c->model_id), v);
    c->pinned   = dsc_stmt_int(st, 5);
    c->last_seq = dsc_stmt_ll(st, 6);
    if ((v = dsc_stmt_text(st, 7))) dsc_strlcpy_utf8(c->created_at, sizeof(c->created_at), v);
    if ((v = dsc_stmt_text(st, 8))) dsc_strlcpy_utf8(c->updated_at, sizeof(c->updated_at), v);
}

#define CONV_COLS "id, mode, title, IFNULL(persona_id,''), model_id, pinned, last_seq, created_at, updated_at"

int dsc_conv_create(dsc_db *db, dsc_mode mode, const char *title, const char *persona_id,
                    const char *model_id, dsc_conversation *out)
{
    if (!db || !model_id || !*model_id) return DSC_E_INVAL;
    const char *mode_s = dsc_mode_name(mode);
    if (!mode_s) return DSC_E_INVAL;
    if (mode == DSC_MODE_CHAT) {
        dsc_conversation exist;
        if (dsc_conv_get_chat(db, &exist) == DSC_OK) return DSC_E_CONFLICT;
    }
    dsc_conversation c;
    memset(&c, 0, sizeof(c));
    dsc_uuid4(c.id);
    dsc_strlcpy_utf8(c.mode, sizeof(c.mode), mode_s);
    dsc_strlcpy_utf8(c.model_id, sizeof(c.model_id), model_id);
    if (title && *title) dsc_strlcpy_utf8(c.title, sizeof(c.title), title);
    else if (mode == DSC_MODE_CHAT) dsc_strlcpy_utf8(c.title, sizeof(c.title), "聊天");
    else dsc_strlcpy_utf8(c.title, sizeof(c.title), "新对话");
    if (persona_id) dsc_strlcpy_utf8(c.persona_id, sizeof(c.persona_id), persona_id);
    dsc_now_iso8601(c.created_at);
    dsc_strlcpy_utf8(c.updated_at, sizeof(c.updated_at), c.created_at);

    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO conversation (id, mode, title, persona_id, model_id, pinned, last_seq, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 0, 0, ?6, ?6);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, c.id);
    dsc_stmt_bind_text(st, 2, c.mode);
    dsc_stmt_bind_text(st, 3, c.title);
    dsc_stmt_bind_text(st, 4, c.persona_id[0] ? c.persona_id : NULL);
    dsc_stmt_bind_text(st, 5, c.model_id);
    dsc_stmt_bind_text(st, 6, c.created_at);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) return rc == DSC_E_CONFLICT ? DSC_E_CONFLICT : rc;
    if (out) *out = c;
    return DSC_OK;
}

int dsc_conv_get(dsc_db *db, const char *id, dsc_conversation *out)
{
    if (!db || !id || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " CONV_COLS " FROM conversation WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_row(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_conv_get_chat(dsc_db *db, dsc_conversation *out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " CONV_COLS " FROM conversation WHERE mode = 'chat' ORDER BY created_at ASC LIMIT 1;");
    if (!st) return DSC_E_DB;
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_row(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_conv_ensure_chat(dsc_db *db, const char *model_id, const char *persona_id,
                         dsc_conversation *out)
{
    int rc = dsc_conv_get_chat(db, out);
    if (rc == DSC_OK) {
        /* 已有会话：补齐人设/模型缺省值，不改动用户已选值。 */
        if ((!out->persona_id[0] && persona_id && *persona_id) ||
            (!out->model_id[0] && model_id && *model_id)) {
            const char *p = out->persona_id[0] ? out->persona_id : persona_id;
            const char *m = out->model_id[0] ? out->model_id : model_id;
            dsc_conv_update_meta(db, out->id, out->title, m, p);
            if (!out->persona_id[0] && p) dsc_strlcpy_utf8(out->persona_id, sizeof(out->persona_id), p);
            if (!out->model_id[0] && m) dsc_strlcpy_utf8(out->model_id, sizeof(out->model_id), m);
        }
        return DSC_OK;
    }
    if (rc != DSC_E_NOTFOUND) return rc;
    return dsc_conv_create(db, DSC_MODE_CHAT, "聊天", persona_id,
                           model_id && *model_id ? model_id : DSC_MODEL_FLASH, out);
}

int dsc_conv_update_meta(dsc_db *db, const char *id, const char *title,
                         const char *model_id, const char *persona_id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_conversation cur;
    int rc = dsc_conv_get(db, id, &cur);
    if (rc != DSC_OK) return rc;
    const char *t = (title && *title) ? title : cur.title;
    const char *m = (model_id && *model_id) ? model_id : cur.model_id;
    const char *p = (persona_id && *persona_id) ? persona_id : cur.persona_id;
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE conversation SET title = ?2, model_id = ?3, persona_id = ?4 WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, t);
    dsc_stmt_bind_text(st, 3, m);
    dsc_stmt_bind_text(st, 4, p && *p ? p : NULL);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_conv_set_pinned(dsc_db *db, const char *id, int pinned)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "UPDATE conversation SET pinned = ?2 WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_ll(st, 2, pinned ? 1 : 0);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_conv_touch(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    char now[DSC_TS_BYTES];
    dsc_now_iso8601(now);
    dsc_stmt *st = dsc_db_prepare(db, "UPDATE conversation SET updated_at = ?2 WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, now);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_conv_delete(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_conversation c;
    int rc = dsc_conv_get(db, id, &c);
    if (rc != DSC_OK) return rc;
    if (strcmp(c.mode, "chat") == 0) return DSC_E_STATE; /* 聊天模式唯一会话不可删除 */
    dsc_stmt *st = dsc_db_prepare(db, "DELETE FROM conversation WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

static int collect(dsc_db *db, dsc_stmt *st, dsc_conversation **out, size_t *out_n)
{
    size_t cap = 8, n = 0;
    dsc_conversation *arr = (dsc_conversation *)dsc_xcalloc(cap, sizeof(dsc_conversation));
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (dsc_conversation *)dsc_xrealloc(arr, cap * sizeof(dsc_conversation));
        }
        read_row(st, &arr[n++]);
    }
    (void)db;
    if (rc != DSC_DONE) {
        dsc_xfree(arr);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

int dsc_conv_list(dsc_db *db, int mode, dsc_conversation **out, size_t *out_n)
{
    if (!db || !out || !out_n) return DSC_E_INVAL;
    const char *sql_all =
        "SELECT " CONV_COLS " FROM conversation ORDER BY pinned DESC, updated_at DESC;";
    const char *sql_mode =
        "SELECT " CONV_COLS " FROM conversation WHERE mode = ?1 ORDER BY pinned DESC, updated_at DESC;";
    dsc_stmt *st = dsc_db_prepare(db, mode < 0 ? sql_all : sql_mode);
    if (!st) return DSC_E_DB;
    if (mode >= 0) dsc_stmt_bind_text(st, 1, dsc_mode_name((dsc_mode)mode));
    int rc = collect(db, st, out, out_n);
    dsc_stmt_finalize(st);
    return rc;
}

int dsc_conv_search(dsc_db *db, int mode, const char *keyword, dsc_conversation **out, size_t *out_n)
{
    if (!db || !keyword || !out || !out_n) return DSC_E_INVAL;
    sb pat;
    sb_init(&pat);
    sb_appendc(&pat, '%');
    sb_append(&pat, keyword);
    sb_appendc(&pat, '%');
    const char *sql_all =
        "SELECT " CONV_COLS " FROM conversation WHERE title LIKE ?1 ORDER BY pinned DESC, updated_at DESC;";
    const char *sql_mode =
        "SELECT " CONV_COLS " FROM conversation WHERE mode = ?2 AND title LIKE ?1 ORDER BY pinned DESC, updated_at DESC;";
    dsc_stmt *st = dsc_db_prepare(db, mode < 0 ? sql_all : sql_mode);
    if (!st) { sb_free(&pat); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, sb_cstr(&pat));
    if (mode >= 0) dsc_stmt_bind_text(st, 2, dsc_mode_name((dsc_mode)mode));
    int rc = collect(db, st, out, out_n);
    dsc_stmt_finalize(st);
    sb_free(&pat);
    return rc;
}

void dsc_conv_free(dsc_conversation *arr, size_t n)
{
    (void)n;
    dsc_xfree(arr);
}
