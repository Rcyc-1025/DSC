/* sync_clock.c —— 设备标识与逻辑时钟实现（技术设计 §16.5，FR-29）。 */
#include "sync_clock.h"
#include "store_setting.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static int ensure_row(dsc_db *db, const char *key, const char *def)
{
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT OR IGNORE INTO setting(key, value) VALUES(?1, ?2);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, key);
    dsc_stmt_bind_text(st, 2, def);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE && rc != DSC_OK) return rc;
    return DSC_OK;
}

int dsc_clock_peek(dsc_db *db, long long *out_ts)
{
    if (!db || !out_ts) return DSC_E_INVAL;
    *out_ts = 0;
    int rc = ensure_row(db, DSC_SET_LOGICAL_TS, "0");
    if (rc != DSC_OK) return rc;
    dsc_stmt *st = dsc_db_prepare(db, "SELECT value FROM setting WHERE key = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, DSC_SET_LOGICAL_TS);
    rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) {
        const char *v = dsc_stmt_text(st, 0);
        long long n = 0;
        if (v && dsc_parse_ll(v, &n) == DSC_OK && n > 0) *out_ts = n;
        rc = DSC_OK;
    } else if (rc == DSC_DONE) {
        rc = DSC_OK;
    }
    dsc_stmt_finalize(st);
    return rc;
}

int dsc_clock_next(dsc_db *db, long long *out_ts)
{
    if (!db || !out_ts) return DSC_E_INVAL;
    *out_ts = 0;
    int rc = ensure_row(db, DSC_SET_LOGICAL_TS, "0");
    if (rc != DSC_OK) return rc;
    /* 单语句自增 + RETURNING：并发写入也不会拿到重复值（SQLite 写锁串行化）。 */
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE setting SET value = CAST(value AS INTEGER) + 1"
        " WHERE key = ?1 RETURNING value;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, DSC_SET_LOGICAL_TS);
    rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) {
        long long n = 0;
        const char *v = dsc_stmt_text(st, 0);
        if (v) dsc_parse_ll(v, &n);
        *out_ts = n;
        rc = DSC_OK;
    } else if (rc == DSC_DONE) {
        rc = DSC_E_STATE;
    }
    dsc_stmt_finalize(st);
    return rc;
}

int dsc_clock_observe(dsc_db *db, long long remote_ts)
{
    if (!db) return DSC_E_INVAL;
    if (remote_ts < 0) remote_ts = 0;
    int rc = ensure_row(db, DSC_SET_LOGICAL_TS, "0");
    if (rc != DSC_OK) return rc;
    /* 只抬高，不降低：远端时钟落后时本地照旧递增，避免本地新消息排到远端历史之前。 */
    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE setting SET value = ?1"
        " WHERE key = ?2 AND CAST(value AS INTEGER) < CAST(?1 AS INTEGER);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_ll(st, 1, remote_ts);
    dsc_stmt_bind_text(st, 2, DSC_SET_LOGICAL_TS);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc == DSC_ROW || rc == DSC_DONE) return DSC_OK;
    return rc;
}

const char *dsc_device_default_name(void)
{
#ifdef _WIN32
    return "Windows 端";
#else
    return "Android 端";
#endif
}

int dsc_device_id(dsc_db *db, char *out, size_t cap)
{
    if (!db || !out || cap == 0) return DSC_E_INVAL;
    out[0] = '\0';
    int rc = ensure_row(db, DSC_SET_DEVICE_ID, "");
    if (rc != DSC_OK) return rc;

    char v[64];
    v[0] = '\0';
    dsc_setting_get(db, DSC_SET_DEVICE_ID, v, sizeof(v));
    if (v[0] == '\0') {
        char gen[40];
        dsc_uuid4(gen);
        /* INSERT OR IGNORE 后重读：两个线程同时初始化时只保留先写入的那个。 */
        dsc_stmt *st = dsc_db_prepare(db,
            "UPDATE setting SET value = ?1 WHERE key = ?2 AND (value IS NULL OR value = '');");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, gen);
        dsc_stmt_bind_text(st, 2, DSC_SET_DEVICE_ID);
        rc = dsc_stmt_step(st);
        dsc_stmt_finalize(st);
        if (rc != DSC_ROW && rc != DSC_DONE) return rc;
        v[0] = '\0';
        dsc_setting_get(db, DSC_SET_DEVICE_ID, v, sizeof(v));
        if (v[0] == '\0') return DSC_E_DB;
    }
    dsc_strlcpy_utf8(out, cap, v);
    return DSC_OK;
}

int dsc_device_name(dsc_db *db, char *out, size_t cap)
{
    if (!db || !out || cap == 0) return DSC_E_INVAL;
    char v[64];
    v[0] = '\0';
    if (dsc_setting_get(db, DSC_SET_DEVICE_NAME, v, sizeof(v)) != DSC_OK || v[0] == '\0') {
        dsc_strlcpy_utf8(out, cap, dsc_device_default_name());
        return DSC_OK;
    }
    dsc_strlcpy_utf8(out, cap, v);
    return DSC_OK;
}

int dsc_device_set_name(dsc_db *db, const char *name)
{
    if (!db || !name) return DSC_E_INVAL;
    if (name[0] == '\0') return dsc_setting_del(db, DSC_SET_DEVICE_NAME);
    if (dsc_utf8_length(name) > 40) return DSC_E_LIMIT;
    return dsc_setting_set(db, DSC_SET_DEVICE_NAME, name);
}
