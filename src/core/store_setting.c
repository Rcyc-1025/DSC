/* store_setting.c */
#include "store_setting.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

int aic_setting_get(aic_db *db, const char *key, char *out, size_t cap)
{
    if (!db || !key || !out || cap == 0) return AIC_E_INVAL;
    out[0] = '\0';
    aic_stmt *st = aic_db_prepare(db, "SELECT value FROM setting WHERE key = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, key);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        const char *v = aic_stmt_text(st, 0);
        if (v) aic_strlcpy_utf8(out, cap, v);
    }
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : (rc == AIC_DONE ? AIC_E_NOTFOUND : rc);
}

int aic_setting_set(aic_db *db, const char *key, const char *value)
{
    if (!db || !key || !*key) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO setting (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, key);
    aic_stmt_bind_text(st, 2, value ? value : "");
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_setting_get_int(aic_db *db, const char *key, int def)
{
    char buf[64];
    if (aic_setting_get(db, key, buf, sizeof(buf)) != AIC_OK) return def;
    int v = def;
    if (aic_parse_int(buf, &v) != AIC_OK) return def;
    return v;
}

int aic_setting_set_int(aic_db *db, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return aic_setting_set(db, key, buf);
}

int aic_setting_del(aic_db *db, const char *key)
{
    if (!db || !key) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "DELETE FROM setting WHERE key = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, key);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_setting_dump_json(aic_db *db, sb *out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "SELECT key, value FROM setting ORDER BY key;");
    if (!st) return AIC_E_DB;
    jw w;
    jw_init(&w, out, 1);
    jw_obj_begin(&w, NULL);
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        jw_put_str(&w, aic_stmt_text(st, 0), aic_stmt_text(st, 1));
    }
    jw_obj_end(&w);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

const char *aic_setting_get_str(aic_db *db, const char *key, const char *def, char *buf, size_t cap)
{
    if (aic_setting_get(db, key, buf, cap) == AIC_OK && buf[0]) return buf;
    aic_strlcpy_utf8(buf, cap, def ? def : "");
    return buf;
}
