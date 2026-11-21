/* store_conversation.h —— 会话存储。mode 字段隔离问答/聊天两种会话（FR-13/FR-14）。 */
#ifndef AIC_STORE_CONVERSATION_H
#define AIC_STORE_CONVERSATION_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_ID_BYTES        40
#define AIC_TITLE_BYTES     201
#define AIC_MODEL_BYTES     64
#define AIC_TS_BYTES        32

typedef struct aic_conversation {
    char      id[AIC_ID_BYTES];
    char      mode[8];            /* "qa" / "chat" */
    char      title[AIC_TITLE_BYTES];
    char      persona_id[AIC_ID_BYTES];
    char      model_id[AIC_MODEL_BYTES];
    int       pinned;
    long long last_seq;
    char      created_at[AIC_TS_BYTES];
    char      updated_at[AIC_TS_BYTES];
} aic_conversation;

/* 新建会话；聊天模式若已存在唯一记录则返回 AIC_E_CONFLICT。 */
int aic_conv_create(aic_db *db, aic_mode mode, const char *title, const char *persona_id,
                    const char *model_id, aic_conversation *out);
int aic_conv_get(aic_db *db, const char *id, aic_conversation *out);
/* 取聊天模式的唯一会话，不存在返回 AIC_E_NOTFOUND。 */
int aic_conv_get_chat(aic_db *db, aic_conversation *out);
/* 取或创建聊天会话（FR-14：始终只有一个活动会话）。 */
int aic_conv_ensure_chat(aic_db *db, const char *model_id, const char *persona_id,
                         aic_conversation *out);
int aic_conv_update_meta(aic_db *db, const char *id, const char *title,
                         const char *model_id, const char *persona_id);
int aic_conv_set_pinned(aic_db *db, const char *id, int pinned);
int aic_conv_touch(aic_db *db, const char *id);
int aic_conv_delete(aic_db *db, const char *id);   /* 聊天会话不允许删除，返回 AIC_E_STATE */

/* 列表：按 pinned DESC, updated_at DESC 排序；mode 传 -1 表示全部。 */
int aic_conv_list(aic_db *db, int mode, aic_conversation **out, size_t *out_n);
/* 标题模糊搜索（问答模式历史搜索）。 */
int aic_conv_search(aic_db *db, int mode, const char *keyword, aic_conversation **out, size_t *out_n);
void aic_conv_free(aic_conversation *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
