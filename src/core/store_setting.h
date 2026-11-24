/* store_setting.h —— 键值设置（FR-17）。 */
#ifndef DSC_STORE_SETTING_H
#define DSC_STORE_SETTING_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 已知设置键。 */
#define DSC_SET_THEME            "ui.theme"            /* day|night|system */
#define DSC_SET_DEFAULT_MODE     "ui.default_mode"     /* qa|chat */
#define DSC_SET_MOTION           "ui.reduce_motion"    /* 0|1 */
#define DSC_SET_MODEL_DEFAULT    "model.default"       /* deepseekv4.1flash|deepseekv4pro */
#define DSC_SET_CONTEXT_N        "chat.context_messages"
#define DSC_SET_RECALL_WINDOW    "chat.recall_window_sec"
#define DSC_SET_STREAM           "chat.stream"
#define DSC_SET_CRED_HAS_KEY     "credential.has_api_key"
#define DSC_SET_CRED_ENDPOINT    "credential.endpoint"
#define DSC_SET_BALANCE_ALERT    "balance.alert_threshold_cents"
#define DSC_SET_UPDATE_AUTO      "update.auto_check"
#define DSC_SET_UPDATE_CHANNEL   "update.channel"
#define DSC_SET_UPDATE_INTERVAL  "update.interval_hours"
#define DSC_SET_FIRST_RUN_DONE   "app.first_run_done"
#define DSC_SET_SCHEMA_APP       "app.schema_version"
#define DSC_SET_PLUGIN_POLICY    "plugin.keep_data_on_uninstall"
/* §7.2 设置项里本轮补齐的三行（知识库 / 设备权限 / 错误中心）。
 * 命名沿用既有命名空间风格（knowledge.* / capability.* / error.*），
 * 与文档示例键（theme / api_endpoint / default_mode）的差异在审计报告里说明：
 * 键名是本实现的稳定契约，改键会破坏已有库，因此以文档为准补语义、以代码为准写键名。 */
#define DSC_SET_KB_BOOKMARK      "knowledge.bookmark"          /* JSON 数组，书签文章 id */
#define DSC_SET_KB_RECENT        "knowledge.recent"            /* JSON 数组，最近阅读，最近在前 */
#define DSC_SET_KB_VERSION_SEEN  "knowledge.version_seen"      /* 用户上次查看的知识包版本 */
#define DSC_SET_KB_CITE          "knowledge.cite"              /* 当前插入的引用片段（进插件注入区，不携全文） */
#define DSC_SET_CAP_PREFIX       "capability."                 /* capability.<能力名> = 1|0 */
#define DSC_SET_ERROR_RETENTION  "error.retention_days"        /* 错误记录保留天数，0 表示不自动清理 */
#define DSC_SET_ERROR_UNREAD     "error.unread"                /* 未读错误条数（状态条角标） */

int dsc_setting_get(dsc_db *db, const char *key, char *out, size_t cap);
int dsc_setting_set(dsc_db *db, const char *key, const char *value);
int dsc_setting_get_int(dsc_db *db, const char *key, int def);
int dsc_setting_set_int(dsc_db *db, const char *key, int value);
int dsc_setting_del(dsc_db *db, const char *key);
/* 取全部设置到 JSON 文本（设置页与导出使用）。 */
int dsc_setting_dump_json(dsc_db *db, sb *out);

/* 带默认值的读取：不存在时返回 def。 */
const char *dsc_setting_get_str(dsc_db *db, const char *key, const char *def, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
