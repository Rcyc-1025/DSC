/* avatar.h —— 头像导入与绘制（技术设计 7.1）。
 *
 * 导入：任意常见图片（PNG/JPEG/BMP/GIF）→ 等比缩到最长边 512 → 存 avatars/<uuid>.jpg
 *       → 返回相对路径 avatars/<uuid>.jpg 写进 user_profile.avatar_path。
 * 绘制：界面把相对路径交给平台层按圆形绘制；失败返回 0，界面退回首字母占位。 */
#ifndef AIC_AVATAR_H
#define AIC_AVATAR_H

#include <stddef.h>

#include "aic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 平台是否支持图片导入（Windows 用 GDI+；Android 由 Java 侧实现，返回 0）。 */
int aic_avatar_available(void);
/* 导入并压缩；out_rel 收到相对路径。 */
int aic_avatar_import(const char *src_path, const char *root, char *out_rel, size_t cap,
                      char *err, size_t errlen);
/* 删除头像文件（路径为相对 root 的路径）；失败忽略。 */
void aic_avatar_remove(const char *root, const char *rel);
/* 把头像画到 hdc 的 (x,y,size,size) 圆形区域；成功返回 1。 */
int aic_avatar_draw(void *hdc, const char *root, const char *rel, int x, int y, int size);

#ifdef __cplusplus
}
#endif

#endif /* AIC_AVATAR_H */
