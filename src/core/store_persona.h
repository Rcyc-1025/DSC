/* store_persona.h —— 陪伴型人设与提示词版本（FR-04）。 */
#ifndef DSC_STORE_PERSONA_H
#define DSC_STORE_PERSONA_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_PERSONA_NAME_BYTES  128
#define DSC_PERSONA_DESC_BYTES  1024
#define DSC_PERSONA_PROMPT_BYTES 65536

typedef struct dsc_persona {
    char      id[40];
    char      name[DSC_PERSONA_NAME_BYTES];
    char      description[DSC_PERSONA_DESC_BYTES];
    char     *base_prompt;      /* 堆分配 */
    int       current_version;
    char      created_at[32];
    char      updated_at[32];
} dsc_persona;

typedef struct dsc_prompt_version {
    char      id[40];
    char      persona_id[40];
    int       version;
    char     *content;          /* 堆分配 */
    char      created_at[32];
} dsc_prompt_version;

int dsc_persona_create(dsc_db *db, const char *name, const char *description,
                       const char *base_prompt, dsc_persona *out);
/* 更新：base_prompt 变化时写入新 prompt_version 并提升 current_version。 */
int dsc_persona_update(dsc_db *db, const char *id, const char *name, const char *description,
                       const char *base_prompt, dsc_persona *out);
int dsc_persona_get(dsc_db *db, const char *id, dsc_persona *out);
/* 默认人设：最早创建的一条；不存在则创建内置默认人设（名字「小鲸鱼」，
 * 正文见 resources/persona/dsc_default_persona.txt）。 */
int dsc_persona_ensure_default(dsc_db *db, dsc_persona *out);
int dsc_persona_list(dsc_db *db, dsc_persona **out, size_t *out_n);
int dsc_persona_versions(dsc_db *db, const char *persona_id, dsc_prompt_version **out, size_t *out_n);
int dsc_persona_delete(dsc_db *db, const char *id);
void dsc_persona_free(dsc_persona *p);
void dsc_persona_free_array(dsc_persona *arr, size_t n);
void dsc_prompt_version_free_array(dsc_prompt_version *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
