/* store_setting.c */
#include "store_setting.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

int dsc_setting_get(dsc_db *db, const char *key, char *out, size_t cap)
{
    if (!db || !key || !out || cap == 0) return DSC_E_INVAL;
    out[0] = '\0';
    dsc_stmt *st = dsc_db_prepare(db, "SELECT value FROM setting WHERE key = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, key);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) {
        const char *v = dsc_stmt_text(st, 0);
        if (v) dsc_strlcpy_utf8(out, cap, v);
    }
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_setting_set(dsc_db *db, const char *key, const char *value)
{
    if (!db || !key || !*key) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO setting (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, key);
    dsc_stmt_bind_text(st, 2, value ? value : "");
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_setting_get_int(dsc_db *db, const char *key, int def)
{
    char buf[64];
    if (dsc_setting_get(db, key, buf, sizeof(buf)) != DSC_OK) return def;
    int v = def;
    if (dsc_parse_int(buf, &v) != DSC_OK) return def;
    return v;
}

int dsc_setting_set_int(dsc_db *db, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return dsc_setting_set(db, key, buf);
}

int dsc_setting_del(dsc_db *db, const char *key)
{
    if (!db || !key) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "DELETE FROM setting WHERE key = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, key);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_setting_dump_json(dsc_db *db, sb *out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "SELECT key, value FROM setting ORDER BY key;");
    if (!st) return DSC_E_DB;
    jw w;
    jw_init(&w, out, 1);
    jw_obj_begin(&w, NULL);
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        jw_put_str(&w, dsc_stmt_text(st, 0), dsc_stmt_text(st, 1));
    }
    jw_obj_end(&w);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

const char *dsc_setting_get_str(dsc_db *db, const char *key, const char *def, char *buf, size_t cap)
{
    if (dsc_setting_get(db, key, buf, cap) == DSC_OK && buf[0]) return buf;
    dsc_strlcpy_utf8(buf, cap, def ? def : "");
    return buf;
}
