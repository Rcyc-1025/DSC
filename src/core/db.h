/* db.h —— SQLite 封装：连接、事务、语句、schema 迁移。
 * 设计要点：
 *  - 所有写入走事务（TDD 要求 db 写入走事务）；
 *  - 迁移按 schema_version 顺序执行，迁移前自动复制到 backups/；
 *  - 语句统一走预编译 + 绑定，避免拼接 SQL。 */
#ifndef DSC_DB_H
#define DSC_DB_H

#include "dsc_types.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsc_db  dsc_db;
typedef struct dsc_stmt dsc_stmt;

/* 打开（必要时创建）数据库；自动设置 PRAGMA。 */
int  dsc_db_open(const char *path, dsc_db **out);
void dsc_db_close(dsc_db *db);
int  dsc_db_exec(dsc_db *db, const char *sql);
const char *dsc_db_errmsg(dsc_db *db);

/* 事务。允许嵌套调用：内层自动转为 SAVEPOINT。 */
int dsc_db_begin(dsc_db *db);
int dsc_db_commit(dsc_db *db);
int dsc_db_rollback(dsc_db *db);

long long dsc_db_last_insert_rowid(dsc_db *db);
int dsc_db_changes(dsc_db *db);

dsc_stmt *dsc_db_prepare(dsc_db *db, const char *sql);
void dsc_stmt_finalize(dsc_stmt *st);
/* 复位语句并清空绑定，便于同一个语句循环复用（批量 INSERT 用）。 */
void dsc_stmt_reset(dsc_stmt *st);

int dsc_stmt_bind_text(dsc_stmt *st, int idx, const char *v);
int dsc_stmt_bind_ll(dsc_stmt *st, int idx, long long v);
int dsc_stmt_bind_null(dsc_stmt *st, int idx);
int dsc_stmt_bind_blob(dsc_stmt *st, int idx, const void *data, size_t size);

/* 返回 DSC_OK 表示有一行，DSC_E_IO 之外无错误；DSC_E_NOTFOUND 表示结束。 */
#define DSC_ROW   DSC_OK
#define DSC_DONE  DSC_E_NOTFOUND
int dsc_stmt_step(dsc_stmt *st);

const char *dsc_stmt_text(dsc_stmt *st, int col);
long long   dsc_stmt_ll(dsc_stmt *st, int col);
int         dsc_stmt_int(dsc_stmt *st, int col);
int         dsc_stmt_is_null(dsc_stmt *st, int col);

/* 便利：执行一个不返回行的语句（绑定 0 个参数）。 */
int dsc_db_exec_stmt(dsc_db *db, const char *sql);

/* ---------------- schema ----------------
 * 当前 schema 版本由 DSC_SCHEMA_VERSION 指定（见 db.c 迁移表）。
 * dsc_db_migrate：把库迁到最新版本；
 *   backup_dir 非空时，迁移前把数据库文件复制一份到该目录。
 *   返回 DSC_OK 表示成功；DSC_E_VERSION 表示库版本高于程序支持。 */
int  dsc_db_schema_version(dsc_db *db, int *out_version);
int  dsc_db_migrate(dsc_db *db, const char *db_path, const char *backup_dir,
                    int *out_from, int *out_to);
int  DSC_SCHEMA_LATEST(void);

#ifdef __cplusplus
}
#endif

#endif /* DSC_DB_H */
