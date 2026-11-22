/* plugin.h —— 插件扩展（FR-05、FR-23）。
 *
 * 形态（与产品裁决一致）：原生动态库插件（Windows DLL / Android .so，纯 C ABI），
 * 跑在独立子进程里，崩溃或超时只终止插件进程，主程序继续（技术设计 §8 / 异常处理章）。
 *
 * 清单 plugin.json：
 *   { "id":"sample_basic", "name":"示例插件", "version":"1.0.0", "author":"...",
 *     "entry":"sample_basic.dll", "permissions":["prompt.fragment"],
 *     "extension_points":[{"point":"prompt.fragment","order":10}] } */
#ifndef AIC_PLUGIN_H
#define AIC_PLUGIN_H

#include <stddef.h>

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_PLUGIN_ID_MAX   64
#define AIC_PLUGIN_NAME_MAX 64
#define AIC_PLUGIN_PATH_MAX 512

/* 插件进程崩溃/超时后的处理结果：终止子进程 + 禁用插件 + 主程序继续。 */
typedef enum aic_plugin_crash_policy {
    AIC_PLUGIN_ON_CRASH_DISABLE = 0,   /* 直接禁用，等用户手动启用 */
    AIC_PLUGIN_ON_CRASH_REPORT       /* 禁用并记录原因 */
} aic_plugin_crash_policy;

typedef struct aic_plugin_manifest {
    char id[AIC_PLUGIN_ID_MAX];
    char name[AIC_PLUGIN_NAME_MAX];
    char version[32];
    char author[AIC_PLUGIN_NAME_MAX];
    char entry[128];                 /* 动态库文件名 */
    char permissions[256];           /* 逗号分隔的权限声明 */
    char extension_points[256];      /* 逗号分隔的扩展点名（按声明顺序拼接） */
    char sha256[65];                 /* 可选：entry 文件的 SHA-256（技术设计 10.2 校验签名或哈希） */
} aic_plugin_manifest;

typedef struct aic_plugin {
    char id[AIC_PLUGIN_ID_MAX];
    char name[AIC_PLUGIN_NAME_MAX];
    char version[32];
    char author[AIC_PLUGIN_NAME_MAX];
    char entry[128];
    char permissions[256];
    char extension_points[256];
    char import_source[16];          /* file / repo */
    char dir[AIC_PLUGIN_PATH_MAX];   /* 安装目录（plugins/<id>） */
    int  enabled;
    char installed_at[32];
} aic_plugin;

/* 解析插件清单；id 必须是字母数字下划线连字符，扩展方以下划线分隔。 */
int aic_plugin_parse_manifest(const char *json, size_t len, aic_plugin_manifest *out,
                              char *err, size_t errlen);

/* 安装：校验清单 → 校验 entry 的 SHA-256（清单里给了才校验）→ 复制文件到 plugins_root/<id>
 * → 写库（默认禁用，等用户启用）。
 * allow_overwrite=1 表示用户确认「覆盖已安装的同名插件」（技术设计 10.2）。 */
int aic_plugin_install(aic_db *db, const char *plugins_root, const char *manifest_path,
                       const char *import_source, aic_plugin *out, char *err, size_t errlen);
int aic_plugin_install_ex(aic_db *db, const char *plugins_root, const char *manifest_path,
                          const char *import_source, int allow_overwrite,
                          aic_plugin *out, char *err, size_t errlen);

int aic_plugin_get(aic_db *db, const char *id, aic_plugin *out);
/* enabled<0 表示全部。 */
int aic_plugin_list(aic_db *db, int enabled, aic_plugin **out, size_t *out_n);
int aic_plugin_set_enabled(aic_db *db, const char *id, int enabled, char *err, size_t errlen);
/* 插件进程崩溃/超时后的处理：禁用插件并记录原因（主程序继续运行，技术设计 §8）。 */
int aic_plugin_disable_after_crash(aic_db *db, const char *id, const char *reason,
                                   char *err, size_t errlen);
/* keep_data=1 只删程序文件，保留插件数据目录。 */
/* 更新已安装插件（覆盖文件并更新版本，保留启用状态与插件数据）。 */
int aic_plugin_update(aic_db *db, const char *plugins_root, const char *manifest_path,
                      aic_plugin *out, char *err, size_t errlen);

/* 调用某个扩展点：对每个声明了该扩展点的已启用插件各调一次，结果汇总成 JSON：
 * {"point":"ui.panel","count":1,"results":[{"plugin":"x","ok":true,"result":{...}}]} */
int aic_plugin_call_point(aic_db *db, const char *host_exe, const char *point, const char *op,
                          const char *request_json, char *out, size_t out_cap,
                          char *err, size_t errlen);

int aic_plugin_uninstall(aic_db *db, const char *id, int keep_data, char *err, size_t errlen);
/* 拼好的扩展点片段（按 order 升序），用于提示词组装；没有启用插件时返回空串。 */
int aic_plugin_build_fragment(aic_db *db, sb *out, char *err, size_t errlen);
void aic_plugin_free(aic_plugin *p);
void aic_plugin_free_array(aic_plugin *arr, size_t n);

/* ---------- 插件宿主（独立进程） ---------- */
/* 清单里声明的扩展点顺序（数值小的先拼接）。 */
int aic_plugin_ext_point_index(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AIC_PLUGIN_H */
