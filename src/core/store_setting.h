/* store_setting.h —— 键值设置（FR-17）。 */
#ifndef AIC_STORE_SETTING_H
#define AIC_STORE_SETTING_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 已知设置键。 */
#define AIC_SET_THEME            "ui.theme"            /* day|night|system */
#define AIC_SET_DEFAULT_MODE     "ui.default_mode"     /* qa|chat */
#define AIC_SET_MOTION           "ui.reduce_motion"    /* 0|1 */
#define AIC_SET_MODEL_DEFAULT    "model.default"       /* deepseekv4.1flash|deepseekv4pro */
#define AIC_SET_CONTEXT_N        "chat.context_messages"
#define AIC_SET_RECALL_WINDOW    "chat.recall_window_sec"
#define AIC_SET_STREAM           "chat.stream"
#define AIC_SET_CRED_HAS_KEY     "credential.has_api_key"
#define AIC_SET_CRED_ENDPOINT    "credential.endpoint"
#define AIC_SET_BALANCE_ALERT    "balance.alert_threshold_cents"
#define AIC_SET_UPDATE_AUTO      "update.auto_check"
#define AIC_SET_UPDATE_CHANNEL   "update.channel"
#define AIC_SET_UPDATE_INTERVAL  "update.interval_hours"
#define AIC_SET_FIRST_RUN_DONE   "app.first_run_done"
#define AIC_SET_SCHEMA_APP       "app.schema_version"
#define AIC_SET_PLUGIN_POLICY    "plugin.keep_data_on_uninstall"

int aic_setting_get(aic_db *db, const char *key, char *out, size_t cap);
int aic_setting_set(aic_db *db, const char *key, const char *value);
int aic_setting_get_int(aic_db *db, const char *key, int def);
int aic_setting_set_int(aic_db *db, const char *key, int value);
int aic_setting_del(aic_db *db, const char *key);
/* 取全部设置到 JSON 文本（设置页与导出使用）。 */
int aic_setting_dump_json(aic_db *db, sb *out);

/* 带默认值的读取：不存在时返回 def。 */
const char *aic_setting_get_str(aic_db *db, const char *key, const char *def, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
