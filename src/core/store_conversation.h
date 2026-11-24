/* store_conversation.h —— 会话存储。mode 字段隔离问答/聊天两种会话（FR-13/FR-14）。 */
#ifndef DSC_STORE_CONVERSATION_H
#define DSC_STORE_CONVERSATION_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_ID_BYTES        40
#define DSC_TITLE_BYTES     201
#define DSC_MODEL_BYTES     64
#define DSC_TS_BYTES        32

typedef struct dsc_conversation {
    char      id[DSC_ID_BYTES];
    char      mode[8];            /* "qa" / "chat" */
    char      title[DSC_TITLE_BYTES];
    char      persona_id[DSC_ID_BYTES];
    char      model_id[DSC_MODEL_BYTES];
    int       pinned;
    long long last_seq;
    char      created_at[DSC_TS_BYTES];
    char      updated_at[DSC_TS_BYTES];
} dsc_conversation;

/* 新建会话；聊天模式若已存在唯一记录则返回 DSC_E_CONFLICT。 */
int dsc_conv_create(dsc_db *db, dsc_mode mode, const char *title, const char *persona_id,
                    const char *model_id, dsc_conversation *out);
int dsc_conv_get(dsc_db *db, const char *id, dsc_conversation *out);
/* 取聊天模式的唯一会话，不存在返回 DSC_E_NOTFOUND。 */
int dsc_conv_get_chat(dsc_db *db, dsc_conversation *out);
/* 取或创建聊天会话（FR-14：始终只有一个活动会话）。 */
int dsc_conv_ensure_chat(dsc_db *db, const char *model_id, const char *persona_id,
                         dsc_conversation *out);
int dsc_conv_update_meta(dsc_db *db, const char *id, const char *title,
                         const char *model_id, const char *persona_id);
int dsc_conv_set_pinned(dsc_db *db, const char *id, int pinned);
int dsc_conv_touch(dsc_db *db, const char *id);
int dsc_conv_delete(dsc_db *db, const char *id);   /* 聊天会话不允许删除，返回 DSC_E_STATE */

/* 列表：按 pinned DESC, updated_at DESC 排序；mode 传 -1 表示全部。 */
int dsc_conv_list(dsc_db *db, int mode, dsc_conversation **out, size_t *out_n);
/* 标题模糊搜索（问答模式历史搜索）。 */
int dsc_conv_search(dsc_db *db, int mode, const char *keyword, dsc_conversation **out, size_t *out_n);
void dsc_conv_free(dsc_conversation *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
