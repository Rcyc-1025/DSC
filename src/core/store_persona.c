/* store_persona.c */
#include "store_persona.h"

#include <stdio.h>
#include <string.h>

#define PERSONA_DEFAULT_PROMPT \
    "你是一个有耐心的陪伴型 AI 助手，说话自然、简短、有温度。\n" \
    "优先回应对方的情绪，其次才是信息；不确定的事不要编造。\n" \
    "用户信息：姓名 {user_name}，性别 {user_gender}，生日 {user_birthday}，" \
    "职业 {user_occupation}，补充说明 {user_extra_note}。未填写的变量为空。"

#define PERSONA_COLS "id, name, description, base_prompt, current_version, created_at, updated_at"

void aic_persona_free(aic_persona *p)
{
    if (!p) return;
    aic_xfree(p->base_prompt);
    p->base_prompt = NULL;
}

void aic_persona_free_array(aic_persona *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) aic_persona_free(&arr[i]);
    aic_xfree(arr);
}

void aic_prompt_version_free_array(aic_prompt_version *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) aic_xfree(arr[i].content);
    aic_xfree(arr);
}

static void read_persona(aic_stmt *st, aic_persona *p)
{
    const char *v;
    memset(p, 0, sizeof(*p));
    if ((v = aic_stmt_text(st, 0))) aic_strlcpy_utf8(p->id, sizeof(p->id), v);
    if ((v = aic_stmt_text(st, 1))) aic_strlcpy_utf8(p->name, sizeof(p->name), v);
    if ((v = aic_stmt_text(st, 2))) aic_strlcpy_utf8(p->description, sizeof(p->description), v);
    if ((v = aic_stmt_text(st, 3))) p->base_prompt = aic_strdup(v);
    p->current_version = aic_stmt_int(st, 4);
    if ((v = aic_stmt_text(st, 5))) aic_strlcpy_utf8(p->created_at, sizeof(p->created_at), v);
    if ((v = aic_stmt_text(st, 6))) aic_strlcpy_utf8(p->updated_at, sizeof(p->updated_at), v);
}

static int add_prompt_version(aic_db *db, const char *persona_id, int version, const char *content)
{
    aic_prompt_version pv;
    memset(&pv, 0, sizeof(pv));
    aic_uuid4(pv.id);
    aic_strlcpy_utf8(pv.persona_id, sizeof(pv.persona_id), persona_id);
    pv.version = version;
    aic_now_iso8601(pv.created_at);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO prompt_version (id, persona_id, version, content, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, pv.id);
    aic_stmt_bind_text(st, 2, pv.persona_id);
    aic_stmt_bind_ll(st, 3, pv.version);
    aic_stmt_bind_text(st, 4, content ? content : "");
    aic_stmt_bind_text(st, 5, pv.created_at);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_persona_create(aic_db *db, const char *name, const char *description,
                       const char *base_prompt, aic_persona *out)
{
    if (!db || !name || !*name) return AIC_E_INVAL;
    aic_persona p;
    memset(&p, 0, sizeof(p));
    aic_uuid4(p.id);
    aic_strlcpy_utf8(p.name, sizeof(p.name), name);
    if (description) aic_strlcpy_utf8(p.description, sizeof(p.description), description);
    p.base_prompt = aic_strdup(base_prompt && *base_prompt ? base_prompt : PERSONA_DEFAULT_PROMPT);
    p.current_version = 1;
    aic_now_iso8601(p.created_at);
    aic_strlcpy_utf8(p.updated_at, sizeof(p.updated_at), p.created_at);

    int rc = aic_db_begin(db);
    if (rc != AIC_OK) { aic_persona_free(&p); return rc; }
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO persona (id, name, description, base_prompt, current_version, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    if (!st) { aic_persona_free(&p); aic_db_rollback(db); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, p.id);
    aic_stmt_bind_text(st, 2, p.name);
    aic_stmt_bind_text(st, 3, p.description);
    aic_stmt_bind_text(st, 4, p.base_prompt);
    aic_stmt_bind_ll(st, 5, p.current_version);
    aic_stmt_bind_text(st, 6, p.created_at);
    aic_stmt_bind_text(st, 7, p.updated_at);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc == AIC_DONE) rc = add_prompt_version(db, p.id, 1, p.base_prompt);
    if (rc != AIC_OK) { aic_persona_free(&p); aic_db_rollback(db); return rc; }
    rc = aic_db_commit(db);
    if (rc != AIC_OK) { aic_persona_free(&p); return rc; }
    if (out) *out = p;
    else aic_persona_free(&p);
    return AIC_OK;
}

int aic_persona_update(aic_db *db, const char *id, const char *name, const char *description,
                       const char *base_prompt, aic_persona *out)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_persona cur;
    int rc = aic_persona_get(db, id, &cur);
    if (rc != AIC_OK) return rc;

    const char *n = (name && *name) ? name : cur.name;
    const char *d = description ? description : cur.description;
    const char *b = (base_prompt && *base_prompt) ? base_prompt : cur.base_prompt;
    int prompt_changed = (base_prompt && *base_prompt && strcmp(base_prompt, cur.base_prompt ? cur.base_prompt : "") != 0);
    int new_version = cur.current_version + (prompt_changed ? 1 : 0);
    char now[32];
    aic_now_iso8601(now);

    rc = aic_db_begin(db);
    if (rc != AIC_OK) { aic_persona_free(&cur); return rc; }
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE persona SET name = ?2, description = ?3, base_prompt = ?4, current_version = ?5, "
        "updated_at = ?6 WHERE id = ?1;");
    if (!st) { aic_persona_free(&cur); aic_db_rollback(db); return AIC_E_DB; }
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, n);
    aic_stmt_bind_text(st, 3, d);
    aic_stmt_bind_text(st, 4, b);
    aic_stmt_bind_ll(st, 5, new_version);
    aic_stmt_bind_text(st, 6, now);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc == AIC_DONE) rc = AIC_OK;
    if (rc == AIC_OK && prompt_changed) rc = add_prompt_version(db, id, new_version, b);
    if (rc != AIC_OK) { aic_persona_free(&cur); aic_db_rollback(db); return rc; }
    rc = aic_db_commit(db);
    aic_persona_free(&cur);
    if (rc != AIC_OK) return rc;
    return aic_persona_get(db, id, out);
}

int aic_persona_get(aic_db *db, const char *id, aic_persona *out)
{
    if (!db || !id || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " PERSONA_COLS " FROM persona WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) read_persona(st, out);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : (rc == AIC_DONE ? AIC_E_NOTFOUND : rc);
}

int aic_persona_ensure_default(aic_db *db, aic_persona *out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " PERSONA_COLS " FROM persona ORDER BY created_at ASC LIMIT 1;");
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        read_persona(st, out);
        aic_stmt_finalize(st);
        return AIC_OK;
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return rc;
    return aic_persona_create(db, "默认陪伴人设", "内置默认人设，可在设置里改写。",
                              PERSONA_DEFAULT_PROMPT, out);
}

int aic_persona_list(aic_db *db, aic_persona **out, size_t *out_n)
{
    if (!db || !out || !out_n) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT " PERSONA_COLS " FROM persona ORDER BY created_at ASC;");
    if (!st) return AIC_E_DB;
    size_t cap = 8, n = 0;
    aic_persona *arr = (aic_persona *)aic_xcalloc(cap, sizeof(aic_persona));
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (aic_persona *)aic_xrealloc(arr, cap * sizeof(aic_persona));
        }
        read_persona(st, &arr[n++]);
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) {
        aic_persona_free_array(arr, n);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

int aic_persona_versions(aic_db *db, const char *persona_id, aic_prompt_version **out, size_t *out_n)
{
    if (!db || !persona_id || !out || !out_n) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT id, persona_id, version, content, created_at FROM prompt_version "
        "WHERE persona_id = ?1 ORDER BY version DESC;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, persona_id);
    size_t cap = 8, n = 0;
    aic_prompt_version *arr = (aic_prompt_version *)aic_xcalloc(cap, sizeof(aic_prompt_version));
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (aic_prompt_version *)aic_xrealloc(arr, cap * sizeof(aic_prompt_version));
        }
        aic_prompt_version *pv = &arr[n++];
        memset(pv, 0, sizeof(*pv));
        const char *v;
        if ((v = aic_stmt_text(st, 0))) aic_strlcpy_utf8(pv->id, sizeof(pv->id), v);
        if ((v = aic_stmt_text(st, 1))) aic_strlcpy_utf8(pv->persona_id, sizeof(pv->persona_id), v);
        pv->version = aic_stmt_int(st, 2);
        if ((v = aic_stmt_text(st, 3))) pv->content = aic_strdup(v);
        if ((v = aic_stmt_text(st, 4))) aic_strlcpy_utf8(pv->created_at, sizeof(pv->created_at), v);
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) {
        aic_prompt_version_free_array(arr, n);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

int aic_persona_delete(aic_db *db, const char *id)
{
    if (!db || !id) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "DELETE FROM persona WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}
