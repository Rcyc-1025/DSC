/* store_message.h —— 消息存储（FR-10 撤回/修改、FR-18 导出、FR-24 分页）。 */
#ifndef AIC_STORE_MESSAGE_H
#define AIC_STORE_MESSAGE_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_RECALL_NOTICE "该消息已被撤回"

typedef struct aic_message {
    char      id[40];
    char      conversation_id[40];
    long long seq;
    char      role[16];
    char     *content;            /* 堆分配，用 aic_msg_free 释放 */
    int       recalled;
    char      edited_at[32];
    char     *original_content;   /* 撤回/首次修改前的原文，可为 NULL */
    char      model_id[64];
    char      created_at[32];
} aic_message;

/* 追加消息，自动分配 seq 并更新会话的 last_seq/updated_at。 */
int aic_msg_append(aic_db *db, const char *conv_id, aic_role role, const char *content,
                   const char *model_id, aic_message *out);
int aic_msg_get(aic_db *db, const char *id, aic_message *out);
/* 分页：before_seq<=0 表示取最新一页；返回按 seq 升序排列的一页。 */
int aic_msg_page(aic_db *db, const char *conv_id, long long before_seq, int limit,
                 aic_message **out, size_t *out_n);
/* 取最近 limit 条未撤回消息（上下文），升序返回。 */
int aic_msg_context(aic_db *db, const char *conv_id, int limit,
                    aic_message **out, size_t *out_n);
/* 撤回：置 recalled=1、正文替换为系统提示、原文存入 original_content。 */
int aic_msg_recall(aic_db *db, const char *id);
/* 撤销撤回：恢复原文并清除标记。 */
int aic_msg_unrecall(aic_db *db, const char *id);
/* 修改：写 content + edited_at，首次修改保存原文。 */
int aic_msg_edit(aic_db *db, const char *id, const char *new_content, char *err, size_t errlen);
int aic_msg_delete(aic_db *db, const char *id);
int aic_msg_count(aic_db *db, const char *conv_id, long long *out_count);
/* 会话内消息的创建时间（用于撤回时间窗判断）。 */
int64_t aic_msg_created_ms(aic_db *db, const char *id);

/* 导出用：取会话内全部消息（含已撤回），按 seq 升序。 */
int aic_msg_export_all(aic_db *db, const char *conv_id, aic_message **out, size_t *out_n);

void aic_msg_free(aic_message *m);
void aic_msg_free_array(aic_message *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
