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

void dsc_persona_free(dsc_persona *p)
{
    if (!p) return;
    dsc_xfree(p->base_prompt);
    p->base_prompt = NULL;
}

void dsc_persona_free_array(dsc_persona *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) dsc_persona_free(&arr[i]);
    dsc_xfree(arr);
}

void dsc_prompt_version_free_array(dsc_prompt_version *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) dsc_xfree(arr[i].content);
    dsc_xfree(arr);
}

static void read_persona(dsc_stmt *st, dsc_persona *p)
{
    const char *v;
    memset(p, 0, sizeof(*p));
    if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(p->id, sizeof(p->id), v);
    if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(p->name, sizeof(p->name), v);
    if ((v = dsc_stmt_text(st, 2))) dsc_strlcpy_utf8(p->description, sizeof(p->description), v);
    if ((v = dsc_stmt_text(st, 3))) p->base_prompt = dsc_strdup(v);
    p->current_version = dsc_stmt_int(st, 4);
    if ((v = dsc_stmt_text(st, 5))) dsc_strlcpy_utf8(p->created_at, sizeof(p->created_at), v);
    if ((v = dsc_stmt_text(st, 6))) dsc_strlcpy_utf8(p->updated_at, sizeof(p->updated_at), v);
}

static int add_prompt_version(dsc_db *db, const char *persona_id, int version, const char *content)
{
    dsc_prompt_version pv;
    memset(&pv, 0, sizeof(pv));
    dsc_uuid4(pv.id);
    dsc_strlcpy_utf8(pv.persona_id, sizeof(pv.persona_id), persona_id);
    pv.version = version;
    dsc_now_iso8601(pv.created_at);
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO prompt_version (id, persona_id, version, content, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, pv.id);
    dsc_stmt_bind_text(st, 2, pv.persona_id);
    dsc_stmt_bind_ll(st, 3, pv.version);
    dsc_stmt_bind_text(st, 4, content ? content : "");
    dsc_stmt_bind_text(st, 5, pv.created_at);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_persona_create(dsc_db *db, const char *name, const char *description,
                       const char *base_prompt, dsc_persona *out)
{
    if (!db || !name || !*name) return DSC_E_INVAL;
    dsc_persona p;
    memset(&p, 0, sizeof(p));
    dsc_uuid4(p.id);
    dsc_strlcpy_utf8(p.name, sizeof(p.name), name);
    if (description) dsc_strlcpy_utf8(p.description, sizeof(p.description), description);
    p.base_prompt = dsc_strdup(base_prompt && *base_prompt ? base_prompt : PERSONA_DEFAULT_PROMPT);
    p.current_version = 1;
    dsc_now_iso8601(p.created_at);
    dsc_strlcpy_utf8(p.updated_at, sizeof(p.updated_at), p.created_at);

    int rc = dsc_db_begin(db);
    if (rc != DSC_OK) { dsc_persona_free(&p); return rc; }
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO persona (id, name, description, base_prompt, current_version, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    if (!st) { dsc_persona_free(&p); dsc_db_rollback(db); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, p.id);
    dsc_stmt_bind_text(st, 2, p.name);
    dsc_stmt_bind_text(st, 3, p.description);
    dsc_stmt_bind_text(st, 4, p.base_prompt);
    dsc_stmt_bind_ll(st, 5, p.current_version);
    dsc_stmt_bind_text(st, 6, p.created_at);
    dsc_stmt_bind_text(st, 7, p.updated_at);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc == DSC_DONE) rc = add_prompt_version(db, p.id, 1, p.base_prompt);
    if (rc != DSC_OK) { dsc_persona_free(&p); dsc_db_rollback(db); return rc; }
    rc = dsc_db_commit(db);
    if (rc != DSC_OK) { dsc_persona_free(&p); return rc; }
    if (out) *out = p;
    else dsc_persona_free(&p);
    return DSC_OK;
}

int dsc_persona_update(dsc_db *db, const char *id, const char *name, const char *description,
                       const char *base_prompt, dsc_persona *out)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_persona cur;
    int rc = dsc_persona_get(db, id, &cur);
    if (rc != DSC_OK) return rc;

    const char *n = (name && *name) ? name : cur.name;
    const char *d = description ? description : cur.description;
    const char *b = (base_prompt && *base_prompt) ? base_prompt : cur.base_prompt;
    int prompt_changed = (base_prompt && *base_prompt && strcmp(base_prompt, cur.base_prompt ? cur.base_prompt : "") != 0);
    int new_version = cur.current_version + (prompt_changed ? 1 : 0);
    char now[32];
    dsc_now_iso8601(now);

    rc = dsc_db_begin(db);
    if (rc != DSC_OK) { dsc_persona_free(&cur); return rc; }
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE persona SET name = ?2, description = ?3, base_prompt = ?4, current_version = ?5, "
        "updated_at = ?6 WHERE id = ?1;");
    if (!st) { dsc_persona_free(&cur); dsc_db_rollback(db); return DSC_E_DB; }
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, n);
    dsc_stmt_bind_text(st, 3, d);
    dsc_stmt_bind_text(st, 4, b);
    dsc_stmt_bind_ll(st, 5, new_version);
    dsc_stmt_bind_text(st, 6, now);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc == DSC_DONE) rc = DSC_OK;
    if (rc == DSC_OK && prompt_changed) rc = add_prompt_version(db, id, new_version, b);
    if (rc != DSC_OK) { dsc_persona_free(&cur); dsc_db_rollback(db); return rc; }
    rc = dsc_db_commit(db);
    dsc_persona_free(&cur);
    if (rc != DSC_OK) return rc;
    return dsc_persona_get(db, id, out);
}

int dsc_persona_get(dsc_db *db, const char *id, dsc_persona *out)
{
    if (!db || !id || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " PERSONA_COLS " FROM persona WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_persona(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_persona_ensure_default(dsc_db *db, dsc_persona *out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " PERSONA_COLS " FROM persona ORDER BY created_at ASC LIMIT 1;");
    if (!st) return DSC_E_DB;
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) {
        read_persona(st, out);
        dsc_stmt_finalize(st);
        return DSC_OK;
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) return rc;
    return dsc_persona_create(db, "默认陪伴人设", "内置默认人设，可在设置里改写。",
                              PERSONA_DEFAULT_PROMPT, out);
}

int dsc_persona_list(dsc_db *db, dsc_persona **out, size_t *out_n)
{
    if (!db || !out || !out_n) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " PERSONA_COLS " FROM persona ORDER BY created_at ASC;");
    if (!st) return DSC_E_DB;
    size_t cap = 8, n = 0;
    dsc_persona *arr = (dsc_persona *)dsc_xcalloc(cap, sizeof(dsc_persona));
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (dsc_persona *)dsc_xrealloc(arr, cap * sizeof(dsc_persona));
        }
        read_persona(st, &arr[n++]);
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) {
        dsc_persona_free_array(arr, n);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

int dsc_persona_versions(dsc_db *db, const char *persona_id, dsc_prompt_version **out, size_t *out_n)
{
    if (!db || !persona_id || !out || !out_n) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT id, persona_id, version, content, created_at FROM prompt_version "
        "WHERE persona_id = ?1 ORDER BY version DESC;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, persona_id);
    size_t cap = 8, n = 0;
    dsc_prompt_version *arr = (dsc_prompt_version *)dsc_xcalloc(cap, sizeof(dsc_prompt_version));
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (dsc_prompt_version *)dsc_xrealloc(arr, cap * sizeof(dsc_prompt_version));
        }
        dsc_prompt_version *pv = &arr[n++];
        memset(pv, 0, sizeof(*pv));
        const char *v;
        if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(pv->id, sizeof(pv->id), v);
        if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(pv->persona_id, sizeof(pv->persona_id), v);
        pv->version = dsc_stmt_int(st, 2);
        if ((v = dsc_stmt_text(st, 3))) pv->content = dsc_strdup(v);
        if ((v = dsc_stmt_text(st, 4))) dsc_strlcpy_utf8(pv->created_at, sizeof(pv->created_at), v);
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) {
        dsc_prompt_version_free_array(arr, n);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

int dsc_persona_delete(dsc_db *db, const char *id)
{
    if (!db || !id) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "DELETE FROM persona WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}
