/* transfer.h —— 聊天记录导出与导入（FR-18）。
 * 导出格式固定为 JSON：format="chat_record"，schema_version=1。
 * 校验和：对所有消息按导出顺序依次吸收 id + 0x1F + content + 0x1F 后取 SHA-256，
 * 导入端重新计算并比对，任何一位改动都会导致 AIC_E_CHECKSUM。
 * 备份包（backup.h）是本文件的另一个命名空间，两者解析器互不兼容。 */
#ifndef AIC_TRANSFER_H
#define AIC_TRANSFER_H

#include "aic_types.h"
#include "db.h"
#include "json.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 导入冲突策略。 */
typedef enum aic_conflict_policy {
    AIC_CONFLICT_SKIP = 0,      /* 同 id 已存在则跳过（默认） */
    AIC_CONFLICT_OVERWRITE = 1, /* 同 id 覆盖本地记录 */
    AIC_CONFLICT_KEEP_BOTH = 2  /* 双方都保留，导入方换新 id */
} aic_conflict_policy;

typedef struct aic_import_preview {
    int    schema_version;
    char   exported_at[32];
    char   source_mode[8];
    size_t conversations;
    size_t messages;
    size_t conflict_conversations;
    size_t conflict_messages;
} aic_import_preview;

typedef struct aic_import_result {
    size_t conv_added;
    size_t conv_overwritten;
    size_t conv_skipped;
    size_t conv_renamed;
    size_t msg_added;
    size_t msg_overwritten;
    size_t msg_skipped;
    size_t msg_renamed;
} aic_import_result;

/* 导出为 JSON 文本（写入 out，调用方负责 sb_free）。mode 传 -1 表示全部会话。 */
int aic_export_json(aic_db *db, int mode, sb *out, char *err, size_t errlen);
/* 导出到文件（JSON / Markdown / 纯文本；后两者不参与导入）。 */
int aic_export_write_json(aic_db *db, int mode, const char *path, char *err, size_t errlen);
int aic_export_write_markdown(aic_db *db, int mode, const char *path, char *err, size_t errlen);
int aic_export_write_text(aic_db *db, int mode, const char *path, char *err, size_t errlen);

/* 读取并校验导出文件：先看 format/版本，再验 checksum，最后统计与本地冲突的数量。 */
int aic_transfer_preview(aic_db *db, const char *path, aic_import_preview *out,
                         char *err, size_t errlen);
/* 执行导入：整体一个事务，任何一步失败回滚并给出错误报告。 */
int aic_transfer_import(aic_db *db, const char *path, aic_conflict_policy policy,
                        aic_import_result *out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* AIC_TRANSFER_H */
