/* transfer.h —— 聊天记录导出与导入（FR-18）。
 * 导出格式固定为 JSON：format="chat_record"，schema_version=1。
 * 校验和：对所有消息按导出顺序依次吸收 id + 0x1F + content + 0x1F 后取 SHA-256，
 * 导入端重新计算并比对，任何一位改动都会导致 DSC_E_CHECKSUM。
 * 备份包（backup.h）是本文件的另一个命名空间，两者解析器互不兼容。 */
#ifndef DSC_TRANSFER_H
#define DSC_TRANSFER_H

#include "dsc_types.h"
#include "db.h"
#include "json.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 导入冲突策略。 */
typedef enum dsc_conflict_policy {
    DSC_CONFLICT_SKIP = 0,      /* 同 id 已存在则跳过（默认） */
    DSC_CONFLICT_OVERWRITE = 1, /* 同 id 覆盖本地记录 */
    DSC_CONFLICT_KEEP_BOTH = 2  /* 双方都保留，导入方换新 id */
} dsc_conflict_policy;

typedef struct dsc_import_preview {
    int    schema_version;
    char   exported_at[32];
    char   source_mode[8];
    size_t conversations;
    size_t messages;
    size_t conflict_conversations;
    size_t conflict_messages;
} dsc_import_preview;

typedef struct dsc_import_result {
    size_t conv_added;
    size_t conv_overwritten;
    size_t conv_skipped;
    size_t conv_renamed;
    size_t msg_added;
    size_t msg_overwritten;
    size_t msg_skipped;
    size_t msg_renamed;
} dsc_import_result;

/* 导出范围（技术设计 8.1）：范围可选当前会话、按模式全部、按时间区间；
 * 头像文件默认不导出，用户勾选后才把 avatar_path 指向的图片以 base64 附在 avatars 数组里。 */
typedef struct dsc_export_scope {
    int         scope;            /* 0=按 mode 的全部会话，1=当前会话，2=按时间区间 */
    const char *conv_id;          /* scope=1 时必需 */
    long long   from_ms;          /* scope=2 的起止时间（毫秒时间戳，按消息 created_at 过滤） */
    long long   to_ms;
    int         include_avatars;  /* 1 = 附带头像文件 */
    const char *root;             /* 数据目录（include_avatars=1 时用来定位头像文件） */
} dsc_export_scope;

int dsc_export_json_ex(dsc_db *db, int mode, const dsc_export_scope *sc, sb *out,
                       char *err, size_t errlen);
int dsc_export_write_json_ex(dsc_db *db, int mode, const dsc_export_scope *sc, const char *path,
                             char *err, size_t errlen);

/* 导出为 JSON 文本（写入 out，调用方负责 sb_free）。mode 传 -1 表示全部会话。 */
int dsc_export_json(dsc_db *db, int mode, sb *out, char *err, size_t errlen);
/* 导出到文件（JSON / Markdown / 纯文本；后两者不参与导入）。 */
int dsc_export_write_json(dsc_db *db, int mode, const char *path, char *err, size_t errlen);
int dsc_export_write_markdown_ex(dsc_db *db, int mode, const dsc_export_scope *sc, const char *path, char *err, size_t errlen);
int dsc_export_write_markdown(dsc_db *db, int mode, const char *path, char *err, size_t errlen);
int dsc_export_write_text_ex(dsc_db *db, int mode, const dsc_export_scope *sc, const char *path, char *err, size_t errlen);
int dsc_export_write_text(dsc_db *db, int mode, const char *path, char *err, size_t errlen);

/* 读取并校验导出文件：先看 format/版本，再验 checksum，最后统计与本地冲突的数量。 */
int dsc_transfer_preview(dsc_db *db, const char *path, dsc_import_preview *out,
                         char *err, size_t errlen);
/* 执行导入：整体一个事务，任何一步失败回滚并给出错误报告。 */
int dsc_transfer_import(dsc_db *db, const char *path, dsc_conflict_policy policy,
                        dsc_import_result *out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DSC_TRANSFER_H */
