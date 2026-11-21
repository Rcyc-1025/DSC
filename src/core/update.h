/* update.h —— 版本检测与更新（FR-07）、更新过程数据保护（FR-08）。
 *
 * 分工：本模块只做「解析清单 → 版本比较 → 校验（哈希/大小/版本）→ 备份前置 → 安装」这些可测逻辑，
 * 真正的网络下载通过 aic_update_download_fn 注入（默认不可用），因此整条链路能在无网环境验证。 */
#ifndef AIC_UPDATE_H
#define AIC_UPDATE_H

#include <stddef.h>

#include "aic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aic_update_manifest {
    char      channel[16];        /* stable / beta */
    char      version[32];        /* 目标版本 */
    char      min_app_version[32];/* 可直升的最低版本（更低要整包） */
    char      released_at[32];    /* ISO8601 */
    char      notes[1024];        /* 更新说明（纯文本） */
    char      url[512];           /* 安装包地址 */
    char      sha256[65];         /* 安装包 SHA-256（小写十六进制） */
    long long size;               /* 安装包字节数，<=0 表示不校验 */
    int       mandatory;          /* 1 表示强制更新 */
} aic_update_manifest;

typedef enum aic_update_status {
    AIC_UPD_NOT_CHECKED = 0,
    AIC_UPD_UP_TO_DATE,      /* 已是最新 */
    AIC_UPD_AVAILABLE,       /* 有可用更新 */
    AIC_UPD_SKIPPED,         /* 用户跳过了这个版本 */
    AIC_UPD_TOO_OLD,         /* 当前版本过低，需要整包安装 */
    AIC_UPD_INVALID          /* 清单不可用 */
} aic_update_status;

typedef struct aic_update_plan {
    aic_update_status status;
    char  current_version[32];
    char  target_version[32];
    int   mandatory;
    int   needs_full_package;   /* 当前版本低于 min_app_version */
} aic_update_plan;

/* 解析更新清单；缺必填字段或格式错误返回 AIC_E_FORMAT。 */
int aic_update_parse_manifest(const char *json, size_t len, aic_update_manifest *out,
                              char *err, size_t errlen);

/* 比较清单与当前版本，得出是否需要更新（FR-07）。skip_version 是用户「跳过此版本」的记录。 */
int aic_update_make_plan(const aic_update_manifest *m, const char *current_version,
                         const char *skip_version, aic_update_plan *out);

/* 校验本地安装包：大小 + SHA-256（FR-08；签名由打包工具在安装前校验）。 */
int aic_update_verify_file(const aic_update_manifest *m, const char *path,
                           char *err, size_t errlen);

/* 数据保护前置条件：备份没成功就不允许开始写入（FR-08）。 */
int aic_update_can_apply(int backup_ok, char *err, size_t errlen);

/* 下载注入：把 url 落到 dst；实现方负责超时与断点续传。 */
typedef int (*aic_update_download_fn)(void *ud, const char *url, const char *dst_path,
                                      long long max_size, char *err, size_t errlen);

/* 默认下载实现：明确不可用（本仓库不发起真实网络请求）。 */
int aic_update_download_unavailable(void *ud, const char *url, const char *dst_path,
                                    long long max_size, char *err, size_t errlen);
/* 本地文件「下载」：把已有安装包复制到 dst，供自动化验收使用。 */
int aic_update_download_local_copy(void *ud, const char *url, const char *dst_path,
                                   long long max_size, char *err, size_t errlen);

/* 完整流程：下载 → 校验 → （可选）安装。test_mode=1 时只走到「校验通过」为止，不真装。 */
int aic_update_run(const aic_update_manifest *m, const char *dst_path,
                   aic_update_download_fn dl, void *ud, int backup_ok, int test_mode,
                   char *err, size_t errlen);

/* 周期检查：距离上次检查是否已到间隔（cron 由界面层驱动）。 */
int aic_update_should_check(int64_t last_check_ms, int interval_hours, int64_t now_ms);

/* 完整流程：下载 → 校验 → 平台替换安装（Windows 替换安装 / Android 系统安装器）。
 * app_dir 为当前程序目录，backup_dir 放回滚点。 */
int aic_update_run_install(const aic_update_manifest *m, const char *dst_path,
                           aic_update_download_fn dl, void *ud, int backup_ok,
                           const char *app_dir, const char *backup_dir, volatile int *cancel,
                           char *err, size_t errlen);

#ifdef _WIN32
/* Windows 替换安装：回滚点 → 静默安装 → 失败还原。 */
int aic_update_apply_package(const char *pkg, const char *app_dir, const char *backup_dir,
                             volatile int *cancel, char *err, size_t errlen);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AIC_UPDATE_H */