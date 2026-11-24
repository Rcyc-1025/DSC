/* memory.h —— AI 记忆条目与关键词索引（技术设计 §9.2 / FR-36）。
 *
 * 记忆由"AI 发言"派生：每条记忆都带 source_message_id。AI 发言被修改时，
 * 同一事务内按 source_message_id 物理删除全部 memory_entry 与索引后重新抽取，
 * 不软删除、不留副本；失败整体回滚（见 aic_engine_edit_ai）。
 * 知识库内容不进记忆抽取（§17），撤回的消息也不抽（§9.2）。
 */
#ifndef AIC_MEMORY_H
#define AIC_MEMORY_H

#include <stddef.h>

#include "aic_types.h"
#include "db.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_MEM_CONTENT_MAX   768
#define AIC_MEM_MAX_ENTRIES   12     /* 单条消息最多抽这么多条记忆 */
#define AIC_MEM_CONTEXT_MAX   (2 * 1024)

typedef struct aic_mem_entry {
    char id[40];
    char source_message_id[40];
    char conversation_id[40];
    char content[AIC_MEM_CONTENT_MAX];
    char origin_device[40];
    char created_at[32];
    char updated_at[32];
} aic_mem_entry;

/* 物理删除某条消息派生的全部记忆与索引，返回删除条数。 */
int aic_mem_purge(aic_db *db, const char *source_message_id, int *out_removed);
/* 从 content 抽取记忆写入（不做删除），返回新增条数。origin_device 可为空。 */
int aic_mem_extract(aic_db *db, const char *source_message_id, const char *conversation_id,
                    const char *content, const char *origin_device, int *out_added);
/* 先删后抽，自带事务。调用方若已在事务里，用 purge + extract。 */
int aic_mem_rebuild(aic_db *db, const char *source_message_id, const char *conversation_id,
                    const char *content, const char *origin_device, int *out_added);

int aic_mem_count(aic_db *db);
int aic_mem_count_for(aic_db *db, const char *source_message_id);
int aic_mem_list(aic_db *db, const char *source_message_id, aic_mem_entry *out, int cap, int *out_n);
/* 关键词检索：按命中 token 数降序。query 为空返回最近条目。 */
int aic_mem_search(aic_db *db, const char *query, aic_mem_entry *out, int cap, int *out_n);
/* 注入片段：本会话相关记忆，超出预算截断。无记忆时置空。 */
int aic_mem_context(aic_db *db, const char *conversation_id, sb *out);
/* 清空全部记忆与索引（数据管理页用）。 */
int aic_mem_clear(aic_db *db);

/* 供测试与"为什么抽到这条"用：一句文本是否会被抽成记忆。 */
int aic_mem_sentence_keep(const char *sentence);

#ifdef __cplusplus
}
#endif

#endif /* AIC_MEMORY_H */
