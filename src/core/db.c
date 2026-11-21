/* db.c —— SQLite 封装实现。 */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

struct aic_db {
    sqlite3 *h;
    int      tx_depth;
    char     err[512];
};

struct aic_stmt {
    aic_db   *db;
    sqlite3_stmt *st;
};

static void set_err(aic_db *db, const char *msg)
{
    if (!db) return;
    aic_strlcpy_utf8(db->err, sizeof(db->err), msg ? msg : "");
}

static int rc_to_err(int rc)
{
    /* 扩展结果码（如 SQLITE_CONSTRAINT_UNIQUE=2067）低 8 位就是主码，先归一化再映射。 */
    rc = rc & 0xff;
    switch (rc) {
    case SQLITE_OK:      return AIC_OK;
    case SQLITE_ROW:     return AIC_OK;
    case SQLITE_DONE:    return AIC_OK;
    case SQLITE_CONSTRAINT: return AIC_E_CONFLICT;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:  return AIC_E_BUSY;
    case SQLITE_NOMEM:   return AIC_E_NOMEM;
    case SQLITE_NOTFOUND:return AIC_E_NOTFOUND;
    case SQLITE_TOOBIG:
    case SQLITE_RANGE:   return AIC_E_INVAL;
    default:             return AIC_E_DB;
    }
}

int aic_db_open(const char *path, aic_db **out)
{
    if (!path || !out) return AIC_E_INVAL;
    *out = NULL;
    aic_db *db = (aic_db *)aic_xcalloc(1, sizeof(aic_db));
    int rc = sqlite3_open_v2(path, &db->h,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        sqlite3_close(db->h);
        aic_xfree(db);
        return AIC_E_DB;
    }
    sqlite3_extended_result_codes(db->h, 1);
    sqlite3_busy_timeout(db->h, 5000);
    /* WAL 提升并发读性能；内存库不支持 WAL，失败可忽略。 */
    sqlite3_exec(db->h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db->h, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db->h, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    *out = db;
    return AIC_OK;
}

void aic_db_close(aic_db *db)
{
    if (!db) return;
    if (db->h) sqlite3_close(db->h);
    aic_xfree(db);
}

int aic_db_exec(aic_db *db, const char *sql)
{
    if (!db || !sql) return AIC_E_INVAL;
    char *errmsg = NULL;
    int rc = sqlite3_exec(db->h, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        set_err(db, errmsg ? errmsg : sqlite3_errmsg(db->h));
        sqlite3_free(errmsg);
        return rc_to_err(rc);
    }
    return AIC_OK;
}

const char *aic_db_errmsg(aic_db *db)
{
    return db ? db->err : "";
}

int aic_db_begin(aic_db *db)
{
    if (!db) return AIC_E_INVAL;
    if (db->tx_depth == 0) {
        int rc = sqlite3_exec(db->h, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            set_err(db, sqlite3_errmsg(db->h));
            return rc_to_err(rc);
        }
        db->tx_depth = 1;
        return AIC_OK;
    }
    char sql[64];
    snprintf(sql, sizeof(sql), "SAVEPOINT aic_sp_%d;", db->tx_depth);
    int rc = sqlite3_exec(db->h, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        return rc_to_err(rc);
    }
    db->tx_depth++;
    return AIC_OK;
}

int aic_db_commit(aic_db *db)
{
    if (!db || db->tx_depth == 0) return AIC_E_STATE;
    int rc;
    if (db->tx_depth == 1) {
        rc = sqlite3_exec(db->h, "COMMIT;", NULL, NULL, NULL);
    } else {
        char sql[80];
        snprintf(sql, sizeof(sql), "RELEASE aic_sp_%d;", db->tx_depth - 1);
        rc = sqlite3_exec(db->h, sql, NULL, NULL, NULL);
    }
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        return rc_to_err(rc);
    }
    db->tx_depth--;
    return AIC_OK;
}

int aic_db_rollback(aic_db *db)
{
    if (!db || db->tx_depth == 0) return AIC_E_STATE;
    int rc;
    if (db->tx_depth == 1) {
        rc = sqlite3_exec(db->h, "ROLLBACK;", NULL, NULL, NULL);
    } else {
        char sql[96];
        snprintf(sql, sizeof(sql), "ROLLBACK TO aic_sp_%d;", db->tx_depth - 1);
        rc = sqlite3_exec(db->h, sql, NULL, NULL, NULL);
        if (rc == SQLITE_OK) {
            char rel[96];
            snprintf(rel, sizeof(rel), "RELEASE aic_sp_%d;", db->tx_depth - 1);
            sqlite3_exec(db->h, rel, NULL, NULL, NULL);
        }
    }
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        return rc_to_err(rc);
    }
    db->tx_depth--;
    return AIC_OK;
}

long long aic_db_last_insert_rowid(aic_db *db)
{
    return db ? (long long)sqlite3_last_insert_rowid(db->h) : 0;
}

int aic_db_changes(aic_db *db)
{
    return db ? sqlite3_changes(db->h) : 0;
}

aic_stmt *aic_db_prepare(aic_db *db, const char *sql)
{
    if (!db || !sql) return NULL;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db->h, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        if (st) sqlite3_finalize(st);
        return NULL;
    }
    aic_stmt *s = (aic_stmt *)aic_xcalloc(1, sizeof(aic_stmt));
    s->db = db;
    s->st = st;
    return s;
}

void aic_stmt_reset(aic_stmt *st)
{
    if (!st) return;
    sqlite3_reset(st->st);
    sqlite3_clear_bindings(st->st);
}

void aic_stmt_finalize(aic_stmt *st)
{
    if (!st) return;
    sqlite3_finalize(st->st);
    aic_xfree(st);
}

int aic_stmt_bind_text(aic_stmt *st, int idx, const char *v)
{
    if (!st) return AIC_E_INVAL;
    int rc;
    if (!v) rc = sqlite3_bind_null(st->st, idx);
    else rc = sqlite3_bind_text(st->st, idx, v, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return AIC_OK;
}

int aic_stmt_bind_ll(aic_stmt *st, int idx, long long v)
{
    if (!st) return AIC_E_INVAL;
    int rc = sqlite3_bind_int64(st->st, idx, (sqlite3_int64)v);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return AIC_OK;
}

int aic_stmt_bind_null(aic_stmt *st, int idx)
{
    if (!st) return AIC_E_INVAL;
    int rc = sqlite3_bind_null(st->st, idx);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return AIC_OK;
}

int aic_stmt_bind_blob(aic_stmt *st, int idx, const void *data, size_t size)
{
    if (!st) return AIC_E_INVAL;
    int rc = sqlite3_bind_blob(st->st, idx, data, (int)size, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return AIC_OK;
}

int aic_stmt_step(aic_stmt *st)
{
    if (!st) return AIC_E_INVAL;
    int rc = sqlite3_step(st->st);
    if (rc == SQLITE_ROW) return AIC_ROW;
    if (rc == SQLITE_DONE) return AIC_DONE;
    set_err(st->db, sqlite3_errmsg(st->db->h));
    return rc_to_err(rc);
}

const char *aic_stmt_text(aic_stmt *st, int col)
{
    if (!st) return NULL;
    const unsigned char *t = sqlite3_column_text(st->st, col);
    return t ? (const char *)t : NULL;
}

long long aic_stmt_ll(aic_stmt *st, int col)
{
    return st ? (long long)sqlite3_column_int64(st->st, col) : 0;
}

int aic_stmt_int(aic_stmt *st, int col)
{
    return st ? sqlite3_column_int(st->st, col) : 0;
}

int aic_stmt_is_null(aic_stmt *st, int col)
{
    return st ? (sqlite3_column_type(st->st, col) == SQLITE_NULL) : 1;
}

int aic_db_exec_stmt(aic_db *db, const char *sql)
{
    aic_stmt *st = aic_db_prepare(db, sql);
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}
