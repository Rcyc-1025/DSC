/* db.h —— SQLite 封装：连接、事务、语句、schema 迁移。
 * 设计要点：
 *  - 所有写入走事务（TDD 要求 db 写入走事务）；
 *  - 迁移按 schema_version 顺序执行，迁移前自动复制到 backups/；
 *  - 语句统一走预编译 + 绑定，避免拼接 SQL。 */
#ifndef AIC_DB_H
#define AIC_DB_H

#include "aic_types.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aic_db  aic_db;
typedef struct aic_stmt aic_stmt;

/* 打开（必要时创建）数据库；自动设置 PRAGMA。 */
int  aic_db_open(const char *path, aic_db **out);
void aic_db_close(aic_db *db);
int  aic_db_exec(aic_db *db, const char *sql);
const char *aic_db_errmsg(aic_db *db);

/* 事务。允许嵌套调用：内层自动转为 SAVEPOINT。 */
int aic_db_begin(aic_db *db);
int aic_db_commit(aic_db *db);
int aic_db_rollback(aic_db *db);

long long aic_db_last_insert_rowid(aic_db *db);
int aic_db_changes(aic_db *db);

aic_stmt *aic_db_prepare(aic_db *db, const char *sql);
void aic_stmt_finalize(aic_stmt *st);
/* 复位语句并清空绑定，便于同一个语句循环复用（批量 INSERT 用）。 */
void aic_stmt_reset(aic_stmt *st);

int aic_stmt_bind_text(aic_stmt *st, int idx, const char *v);
int aic_stmt_bind_ll(aic_stmt *st, int idx, long long v);
int aic_stmt_bind_null(aic_stmt *st, int idx);
int aic_stmt_bind_blob(aic_stmt *st, int idx, const void *data, size_t size);

/* 返回 AIC_OK 表示有一行，AIC_E_IO 之外无错误；AIC_E_NOTFOUND 表示结束。 */
#define AIC_ROW   AIC_OK
#define AIC_DONE  AIC_E_NOTFOUND
int aic_stmt_step(aic_stmt *st);

const char *aic_stmt_text(aic_stmt *st, int col);
long long   aic_stmt_ll(aic_stmt *st, int col);
int         aic_stmt_int(aic_stmt *st, int col);
int         aic_stmt_is_null(aic_stmt *st, int col);

/* 便利：执行一个不返回行的语句（绑定 0 个参数）。 */
int aic_db_exec_stmt(aic_db *db, const char *sql);

/* ---------------- schema ----------------
 * 当前 schema 版本由 AIC_SCHEMA_VERSION 指定（见 db.c 迁移表）。
 * aic_db_migrate：把库迁到最新版本；
 *   backup_dir 非空时，迁移前把数据库文件复制一份到该目录。
 *   返回 AIC_OK 表示成功；AIC_E_VERSION 表示库版本高于程序支持。 */
int  aic_db_schema_version(aic_db *db, int *out_version);
int  aic_db_migrate(aic_db *db, const char *db_path, const char *backup_dir,
                    int *out_from, int *out_to);
int  AIC_SCHEMA_LATEST(void);

#ifdef __cplusplus
}
#endif

#endif /* AIC_DB_H */
