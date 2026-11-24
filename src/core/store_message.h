/* store_message.h —— 消息存储（FR-10 撤回/修改、FR-18 导出、FR-24 分页）。 */
#ifndef DSC_STORE_MESSAGE_H
#define DSC_STORE_MESSAGE_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_RECALL_NOTICE "该消息已被撤回"

typedef struct dsc_message {
    char      id[40];
    char      conversation_id[40];
    long long seq;
    char      role[16];
    char     *content;            /* 堆分配，用 dsc_msg_free 释放 */
    int       recalled;
    int       superseded;         /* 1 表示被「重新生成」替换，界面与上下文不再使用 */
    char      edited_at[32];
    char     *original_content;   /* 撤回/首次修改前的原文，可为 NULL */
    char      model_id[64];
    char      created_at[32];
    long long logical_ts;         /* 逻辑时钟值，跨端排序主键（§16.5） */
    char      origin_device[40];  /* 消息产生端设备标识 */
} dsc_message;

/* 追加消息，自动分配 seq 并更新会话的 last_seq/updated_at。 */
int dsc_msg_append(dsc_db *db, const char *conv_id, dsc_role role, const char *content,
                   const char *model_id, dsc_message *out);
int dsc_msg_get(dsc_db *db, const char *id, dsc_message *out);
/* 分页：before_seq<=0 表示取最新一页；返回按 seq 升序排列的一页。 */
int dsc_msg_page(dsc_db *db, const char *conv_id, long long before_seq, int limit,
                 dsc_message **out, size_t *out_n);
/* 跨端分页：按 (logical_ts, origin_device, seq) 排序。
 * before_ts<=0 表示取最后一页；否则取严格小于该三元组的更早一页，结果升序返回。 */
int dsc_msg_page_ordered(dsc_db *db, const char *conv_id,
                         long long before_ts, const char *before_device, long long before_seq,
                         int limit, dsc_message **out, size_t *out_n);
/* 取最近 limit 条未撤回消息（上下文），升序返回。 */
int dsc_msg_context(dsc_db *db, const char *conv_id, int limit,
                    dsc_message **out, size_t *out_n);
/* 撤回：置 recalled=1、正文替换为系统提示、原文存入 original_content。 */
int dsc_msg_recall(dsc_db *db, const char *id);
/* 撤销撤回：恢复原文并清除标记。 */
int dsc_msg_unrecall(dsc_db *db, const char *id);
/* 修改：写 content + edited_at，首次修改保存原文。 */
int dsc_msg_edit(dsc_db *db, const char *id, const char *new_content, char *err, size_t errlen);
int dsc_msg_delete(dsc_db *db, const char *id);
/* 重新生成：把旧回复标记为被替换（保留数据，不删除）。 */
int dsc_msg_supersede(dsc_db *db, const char *id);
/* 撤销修改：正文回滚到 original_content，并清掉 original_content 与 edited_at。 */
int dsc_msg_restore(dsc_db *db, const char *id);
/* 会话内最后一条未被替换、未撤回的助手消息；没有返回 DSC_E_NOTFOUND。 */
/* 最近一条未撤回、未替换的 user 消息（扩展点 message.transform 的输入）。 */
int dsc_msg_last_user(dsc_db *db, const char *conv_id, dsc_message *out);
int dsc_msg_last_assistant(dsc_db *db, const char *conv_id, dsc_message *out);
int dsc_msg_count(dsc_db *db, const char *conv_id, long long *out_count);
/* 会话内消息的创建时间（用于撤回时间窗判断）。 */
int64_t dsc_msg_created_ms(dsc_db *db, const char *id);

/* 导出用：取会话内全部消息（含已撤回），按 seq 升序。 */
int dsc_msg_export_all(dsc_db *db, const char *conv_id, dsc_message **out, size_t *out_n);

void dsc_msg_free(dsc_message *m);
void dsc_msg_free_array(dsc_message *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
