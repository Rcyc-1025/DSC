/* store_persona.h —— 陪伴型人设与提示词版本（FR-04）。 */
#ifndef AIC_STORE_PERSONA_H
#define AIC_STORE_PERSONA_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_PERSONA_NAME_BYTES  128
#define AIC_PERSONA_DESC_BYTES  1024
#define AIC_PERSONA_PROMPT_BYTES 65536

typedef struct aic_persona {
    char      id[40];
    char      name[AIC_PERSONA_NAME_BYTES];
    char      description[AIC_PERSONA_DESC_BYTES];
    char     *base_prompt;      /* 堆分配 */
    int       current_version;
    char      created_at[32];
    char      updated_at[32];
} aic_persona;

typedef struct aic_prompt_version {
    char      id[40];
    char      persona_id[40];
    int       version;
    char     *content;          /* 堆分配 */
    char      created_at[32];
} aic_prompt_version;

int aic_persona_create(aic_db *db, const char *name, const char *description,
                       const char *base_prompt, aic_persona *out);
/* 更新：base_prompt 变化时写入新 prompt_version 并提升 current_version。 */
int aic_persona_update(aic_db *db, const char *id, const char *name, const char *description,
                       const char *base_prompt, aic_persona *out);
int aic_persona_get(aic_db *db, const char *id, aic_persona *out);
/* 默认人设：最早创建的一条；不存在则创建内置默认人设。 */
int aic_persona_ensure_default(aic_db *db, aic_persona *out);
int aic_persona_list(aic_db *db, aic_persona **out, size_t *out_n);
int aic_persona_versions(aic_db *db, const char *persona_id, aic_prompt_version **out, size_t *out_n);
int aic_persona_delete(aic_db *db, const char *id);
void aic_persona_free(aic_persona *p);
void aic_persona_free_array(aic_persona *arr, size_t n);
void aic_prompt_version_free_array(aic_prompt_version *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
