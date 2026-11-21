/* backup.h —— 数据备份包（技术设计 5.5 / 8.3 / 14.2）。
 *
 * 备份包是一个标准 ZIP（store 模式），包含：
 *   manifest.json   包格式、版本、创建时间、db schema、逐文件 SHA-256
 *   db/sai.db       会话与人设数据
 *   avatars/        头像
 *   plugins/        插件数据（与核心目录分离，备份时一并带上）
 * 备份前做一次数据库一致性检查（PRAGMA quick_check）并 checkpoint WAL，保证包内 db 可独立使用。
 * 备份成功是任何写入动作的前置条件（更新安装、迁移）。 */
#ifndef AIC_BACKUP_H
#define AIC_BACKUP_H

#include <stddef.h>

#include "aic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aic_backup_info {
    int       schema_version;
    char      created_at[32];
    char      app_version[32];
    int       db_schema;
    int       file_count;
    long long total_bytes;
} aic_backup_info;

/* 生成备份包；out 可为 NULL。 */
int aic_backup_create(const char *root, const char *db_path, const char *app_version,
                      const char *out_zip, aic_backup_info *out, char *err, size_t errlen);
/* 读取备份包清单（不做哈希校验）。 */
int aic_backup_inspect(const char *zip_path, aic_backup_info *out, char *err, size_t errlen);
/* 校验备份包：清单可解析、每个文件存在且 SHA-256 相符。 */
int aic_backup_verify(const char *zip_path, aic_backup_info *out, char *err, size_t errlen);
/* 从备份包恢复：解到临时目录 → 校验 → 替换 db、avatars、plugins。 */
int aic_backup_restore(const char *zip_path, const char *root, char *err, size_t errlen);
/* 清理旧备份，保留最新 keep 个；removed 可为 NULL。 */
int aic_backup_clean(const char *backups_dir, int keep, int *removed, char *err, size_t errlen);
/* 目录下最新的备份包路径；没有返回 AIC_E_NOTFOUND。 */
int aic_backup_latest(const char *backups_dir, char *out_path, size_t cap,
                      aic_backup_info *out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* AIC_BACKUP_H */
