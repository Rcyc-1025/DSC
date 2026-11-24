/* db.c —— SQLite 封装实现。 */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

struct dsc_db {
    sqlite3 *h;
    int      tx_depth;
    char     err[512];
};

struct dsc_stmt {
    dsc_db   *db;
    sqlite3_stmt *st;
};

static void set_err(dsc_db *db, const char *msg)
{
    if (!db) return;
    dsc_strlcpy_utf8(db->err, sizeof(db->err), msg ? msg : "");
}

static int rc_to_err(int rc)
{
    /* 扩展结果码（如 SQLITE_CONSTRAINT_UNIQUE=2067）低 8 位就是主码，先归一化再映射。 */
    rc = rc & 0xff;
    switch (rc) {
    case SQLITE_OK:      return DSC_OK;
    case SQLITE_ROW:     return DSC_OK;
    case SQLITE_DONE:    return DSC_OK;
    case SQLITE_CONSTRAINT: return DSC_E_CONFLICT;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:  return DSC_E_BUSY;
    case SQLITE_NOMEM:   return DSC_E_NOMEM;
    case SQLITE_NOTFOUND:return DSC_E_NOTFOUND;
    case SQLITE_TOOBIG:
    case SQLITE_RANGE:   return DSC_E_INVAL;
    default:             return DSC_E_DB;
    }
}

int dsc_db_open(const char *path, dsc_db **out)
{
    if (!path || !out) return DSC_E_INVAL;
    *out = NULL;
    dsc_db *db = (dsc_db *)dsc_xcalloc(1, sizeof(dsc_db));
    int rc = sqlite3_open_v2(path, &db->h,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        sqlite3_close(db->h);
        dsc_xfree(db);
        return DSC_E_DB;
    }
    sqlite3_extended_result_codes(db->h, 1);
    sqlite3_busy_timeout(db->h, 5000);
    /* WAL 提升并发读性能；内存库不支持 WAL，失败可忽略。 */
    sqlite3_exec(db->h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db->h, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db->h, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    *out = db;
    return DSC_OK;
}

void dsc_db_close(dsc_db *db)
{
    if (!db) return;
    if (db->h) sqlite3_close(db->h);
    dsc_xfree(db);
}

int dsc_db_exec(dsc_db *db, const char *sql)
{
    if (!db || !sql) return DSC_E_INVAL;
    char *errmsg = NULL;
    int rc = sqlite3_exec(db->h, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        set_err(db, errmsg ? errmsg : sqlite3_errmsg(db->h));
        sqlite3_free(errmsg);
        return rc_to_err(rc);
    }
    return DSC_OK;
}

const char *dsc_db_errmsg(dsc_db *db)
{
    return db ? db->err : "";
}

int dsc_db_begin(dsc_db *db)
{
    if (!db) return DSC_E_INVAL;
    if (db->tx_depth == 0) {
        int rc = sqlite3_exec(db->h, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            set_err(db, sqlite3_errmsg(db->h));
            return rc_to_err(rc);
        }
        db->tx_depth = 1;
        return DSC_OK;
    }
    char sql[64];
    snprintf(sql, sizeof(sql), "SAVEPOINT dsc_sp_%d;", db->tx_depth);
    int rc = sqlite3_exec(db->h, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        return rc_to_err(rc);
    }
    db->tx_depth++;
    return DSC_OK;
}

int dsc_db_commit(dsc_db *db)
{
    if (!db || db->tx_depth == 0) return DSC_E_STATE;
    int rc;
    if (db->tx_depth == 1) {
        rc = sqlite3_exec(db->h, "COMMIT;", NULL, NULL, NULL);
    } else {
        char sql[80];
        snprintf(sql, sizeof(sql), "RELEASE dsc_sp_%d;", db->tx_depth - 1);
        rc = sqlite3_exec(db->h, sql, NULL, NULL, NULL);
    }
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        return rc_to_err(rc);
    }
    db->tx_depth--;
    return DSC_OK;
}

int dsc_db_rollback(dsc_db *db)
{
    if (!db || db->tx_depth == 0) return DSC_E_STATE;
    int rc;
    if (db->tx_depth == 1) {
        rc = sqlite3_exec(db->h, "ROLLBACK;", NULL, NULL, NULL);
    } else {
        char sql[96];
        snprintf(sql, sizeof(sql), "ROLLBACK TO dsc_sp_%d;", db->tx_depth - 1);
        rc = sqlite3_exec(db->h, sql, NULL, NULL, NULL);
        if (rc == SQLITE_OK) {
            char rel[96];
            snprintf(rel, sizeof(rel), "RELEASE dsc_sp_%d;", db->tx_depth - 1);
            sqlite3_exec(db->h, rel, NULL, NULL, NULL);
        }
    }
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        return rc_to_err(rc);
    }
    db->tx_depth--;
    return DSC_OK;
}

long long dsc_db_last_insert_rowid(dsc_db *db)
{
    return db ? (long long)sqlite3_last_insert_rowid(db->h) : 0;
}

int dsc_db_changes(dsc_db *db)
{
    return db ? sqlite3_changes(db->h) : 0;
}

dsc_stmt *dsc_db_prepare(dsc_db *db, const char *sql)
{
    if (!db || !sql) return NULL;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db->h, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        set_err(db, sqlite3_errmsg(db->h));
        if (st) sqlite3_finalize(st);
        return NULL;
    }
    dsc_stmt *s = (dsc_stmt *)dsc_xcalloc(1, sizeof(dsc_stmt));
    s->db = db;
    s->st = st;
    return s;
}

void dsc_stmt_reset(dsc_stmt *st)
{
    if (!st) return;
    sqlite3_reset(st->st);
    sqlite3_clear_bindings(st->st);
}

void dsc_stmt_finalize(dsc_stmt *st)
{
    if (!st) return;
    sqlite3_finalize(st->st);
    dsc_xfree(st);
}

int dsc_stmt_bind_text(dsc_stmt *st, int idx, const char *v)
{
    if (!st) return DSC_E_INVAL;
    int rc;
    if (!v) rc = sqlite3_bind_null(st->st, idx);
    else rc = sqlite3_bind_text(st->st, idx, v, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return DSC_OK;
}

int dsc_stmt_bind_ll(dsc_stmt *st, int idx, long long v)
{
    if (!st) return DSC_E_INVAL;
    int rc = sqlite3_bind_int64(st->st, idx, (sqlite3_int64)v);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return DSC_OK;
}

int dsc_stmt_bind_null(dsc_stmt *st, int idx)
{
    if (!st) return DSC_E_INVAL;
    int rc = sqlite3_bind_null(st->st, idx);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return DSC_OK;
}

int dsc_stmt_bind_blob(dsc_stmt *st, int idx, const void *data, size_t size)
{
    if (!st) return DSC_E_INVAL;
    int rc = sqlite3_bind_blob(st->st, idx, data, (int)size, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        set_err(st->db, sqlite3_errmsg(st->db->h));
        return rc_to_err(rc);
    }
    return DSC_OK;
}

int dsc_stmt_step(dsc_stmt *st)
{
    if (!st) return DSC_E_INVAL;
    int rc = sqlite3_step(st->st);
    if (rc == SQLITE_ROW) return DSC_ROW;
    if (rc == SQLITE_DONE) return DSC_DONE;
    set_err(st->db, sqlite3_errmsg(st->db->h));
    return rc_to_err(rc);
}

const char *dsc_stmt_text(dsc_stmt *st, int col)
{
    if (!st) return NULL;
    const unsigned char *t = sqlite3_column_text(st->st, col);
    return t ? (const char *)t : NULL;
}

long long dsc_stmt_ll(dsc_stmt *st, int col)
{
    return st ? (long long)sqlite3_column_int64(st->st, col) : 0;
}

int dsc_stmt_int(dsc_stmt *st, int col)
{
    return st ? sqlite3_column_int(st->st, col) : 0;
}

int dsc_stmt_is_null(dsc_stmt *st, int col)
{
    return st ? (sqlite3_column_type(st->st, col) == SQLITE_NULL) : 1;
}

int dsc_db_exec_stmt(dsc_db *db, const char *sql)
{
    dsc_stmt *st = dsc_db_prepare(db, sql);
    if (!st) return DSC_E_DB;
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}
