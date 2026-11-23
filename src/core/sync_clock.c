/* sync_clock.c —— 设备标识与逻辑时钟实现（技术设计 §16.5，FR-29）。 */
#include "sync_clock.h"
#include "store_setting.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static int ensure_row(aic_db *db, const char *key, const char *def)
{
    aic_stmt *st = aic_db_prepare(db,
        "INSERT OR IGNORE INTO setting(key, value) VALUES(?1, ?2);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, key);
    aic_stmt_bind_text(st, 2, def);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE && rc != AIC_OK) return rc;
    return AIC_OK;
}

int aic_clock_peek(aic_db *db, long long *out_ts)
{
    if (!db || !out_ts) return AIC_E_INVAL;
    *out_ts = 0;
    int rc = ensure_row(db, AIC_SET_LOGICAL_TS, "0");
    if (rc != AIC_OK) return rc;
    aic_stmt *st = aic_db_prepare(db, "SELECT value FROM setting WHERE key = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, AIC_SET_LOGICAL_TS);
    rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        const char *v = aic_stmt_text(st, 0);
        long long n = 0;
        if (v && aic_parse_ll(v, &n) == AIC_OK && n > 0) *out_ts = n;
        rc = AIC_OK;
    } else if (rc == AIC_DONE) {
        rc = AIC_OK;
    }
    aic_stmt_finalize(st);
    return rc;
}

int aic_clock_next(aic_db *db, long long *out_ts)
{
    if (!db || !out_ts) return AIC_E_INVAL;
    *out_ts = 0;
    int rc = ensure_row(db, AIC_SET_LOGICAL_TS, "0");
    if (rc != AIC_OK) return rc;
    /* 单语句自增 + RETURNING：并发写入也不会拿到重复值（SQLite 写锁串行化）。 */
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE setting SET value = CAST(value AS INTEGER) + 1"
        " WHERE key = ?1 RETURNING value;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, AIC_SET_LOGICAL_TS);
    rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        long long n = 0;
        const char *v = aic_stmt_text(st, 0);
        if (v) aic_parse_ll(v, &n);
        *out_ts = n;
        rc = AIC_OK;
    } else if (rc == AIC_DONE) {
        rc = AIC_E_STATE;
    }
    aic_stmt_finalize(st);
    return rc;
}

int aic_clock_observe(aic_db *db, long long remote_ts)
{
    if (!db) return AIC_E_INVAL;
    if (remote_ts < 0) remote_ts = 0;
    int rc = ensure_row(db, AIC_SET_LOGICAL_TS, "0");
    if (rc != AIC_OK) return rc;
    /* 只抬高，不降低：远端时钟落后时本地照旧递增，避免本地新消息排到远端历史之前。 */
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE setting SET value = ?1"
        " WHERE key = ?2 AND CAST(value AS INTEGER) < CAST(?1 AS INTEGER);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_ll(st, 1, remote_ts);
    aic_stmt_bind_text(st, 2, AIC_SET_LOGICAL_TS);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc == AIC_ROW || rc == AIC_DONE) return AIC_OK;
    return rc;
}

const char *aic_device_default_name(void)
{
#ifdef _WIN32
    return "Windows 端";
#else
    return "Android 端";
#endif
}

int aic_device_id(aic_db *db, char *out, size_t cap)
{
    if (!db || !out || cap == 0) return AIC_E_INVAL;
    out[0] = '\0';
    int rc = ensure_row(db, AIC_SET_DEVICE_ID, "");
    if (rc != AIC_OK) return rc;

    char v[64];
    v[0] = '\0';
    aic_setting_get(db, AIC_SET_DEVICE_ID, v, sizeof(v));
    if (v[0] == '\0') {
        char gen[40];
        aic_uuid4(gen);
        /* INSERT OR IGNORE 后重读：两个线程同时初始化时只保留先写入的那个。 */
        aic_stmt *st = aic_db_prepare(db,
            "UPDATE setting SET value = ?1 WHERE key = ?2 AND (value IS NULL OR value = '');");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, gen);
        aic_stmt_bind_text(st, 2, AIC_SET_DEVICE_ID);
        rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        if (rc != AIC_ROW && rc != AIC_DONE) return rc;
        v[0] = '\0';
        aic_setting_get(db, AIC_SET_DEVICE_ID, v, sizeof(v));
        if (v[0] == '\0') return AIC_E_DB;
    }
    aic_strlcpy_utf8(out, cap, v);
    return AIC_OK;
}

int aic_device_name(aic_db *db, char *out, size_t cap)
{
    if (!db || !out || cap == 0) return AIC_E_INVAL;
    char v[64];
    v[0] = '\0';
    if (aic_setting_get(db, AIC_SET_DEVICE_NAME, v, sizeof(v)) != AIC_OK || v[0] == '\0') {
        aic_strlcpy_utf8(out, cap, aic_device_default_name());
        return AIC_OK;
    }
    aic_strlcpy_utf8(out, cap, v);
    return AIC_OK;
}

int aic_device_set_name(aic_db *db, const char *name)
{
    if (!db || !name) return AIC_E_INVAL;
    if (name[0] == '\0') return aic_setting_del(db, AIC_SET_DEVICE_NAME);
    if (aic_utf8_length(name) > 40) return AIC_E_LIMIT;
    return aic_setting_set(db, AIC_SET_DEVICE_NAME, name);
}
