/* store_profile.h —— 用户资料（FR-16）。单行表，id 固定为 1。 */
#ifndef AIC_STORE_PROFILE_H
#define AIC_STORE_PROFILE_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_PROFILE_NAME_BYTES     (AIC_PROFILE_NAME_MAX * 4 + 1)
#define AIC_PROFILE_OCC_BYTES      (AIC_PROFILE_OCCUPATION_MAX * 4 + 1)
#define AIC_PROFILE_NOTE_BYTES     (AIC_PROFILE_NOTE_MAX * 4 + 1)
#define AIC_PROFILE_PATH_BYTES     512

typedef struct aic_profile {
    char name[AIC_PROFILE_NAME_BYTES];        /* 最长 40 字符 */
    char avatar_path[AIC_PROFILE_PATH_BYTES]; /* 本地相对路径，禁止外链 */
    char gender[16];                          /* 未设置 / 男 / 女 / 其他 */
    char birthday[16];                        /* YYYY-MM-DD 或空 */
    char occupation[AIC_PROFILE_OCC_BYTES];   /* 最长 60 字符 */
    char extra_note[AIC_PROFILE_NOTE_BYTES];  /* 最长 500 字符 */
    char updated_at[40];
} aic_profile;

int aic_profile_validate(const aic_profile *p, char *err, size_t errlen);
/* 读取；未初始化时返回全空的默认资料（AIC_OK）。 */
int aic_profile_get(aic_db *db, aic_profile *out);
/* 写入（UPSERT）。校验失败返回 AIC_E_INVAL 并写 err。 */
int aic_profile_set(aic_db *db, const aic_profile *in, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
