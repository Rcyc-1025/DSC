/* store_profile.h —— 用户资料（FR-16）。单行表，id 固定为 1。 */
#ifndef DSC_STORE_PROFILE_H
#define DSC_STORE_PROFILE_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_PROFILE_NAME_BYTES     (DSC_PROFILE_NAME_MAX * 4 + 1)
#define DSC_PROFILE_OCC_BYTES      (DSC_PROFILE_OCCUPATION_MAX * 4 + 1)
#define DSC_PROFILE_NOTE_BYTES     (DSC_PROFILE_NOTE_MAX * 4 + 1)
#define DSC_PROFILE_PATH_BYTES     512

typedef struct dsc_profile {
    char name[DSC_PROFILE_NAME_BYTES];        /* 最长 40 字符 */
    char avatar_path[DSC_PROFILE_PATH_BYTES]; /* 本地相对路径，禁止外链 */
    char gender[16];                          /* 未设置 / 男 / 女 / 其他 */
    char birthday[16];                        /* YYYY-MM-DD 或空 */
    char occupation[DSC_PROFILE_OCC_BYTES];   /* 最长 60 字符 */
    char extra_note[DSC_PROFILE_NOTE_BYTES];  /* 最长 500 字符 */
    char updated_at[40];
} dsc_profile;

int dsc_profile_validate(const dsc_profile *p, char *err, size_t errlen);
/* 读取；未初始化时返回全空的默认资料（DSC_OK）。 */
int dsc_profile_get(dsc_db *db, dsc_profile *out);
/* 写入（UPSERT）。校验失败返回 DSC_E_INVAL 并写 err。 */
int dsc_profile_set(dsc_db *db, const dsc_profile *in, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
