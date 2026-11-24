/* capability.h —— 设备能力与授权（技术设计 §18 / FR-35）。
 *
 * 六项能力：时间 / 位置 / 天气 / 设备图片 / 已安装软件列表 / 本地文件。
 * 授权模型（§18）：
 *   - 时间必需：首次启动申请，未授予不进主页；授予后不可撤销；
 *   - 其余五项按需申请、可单独授予或撤销，申请前必须先展示用途；
 *   - 拒绝不影响主对话：对话、同步、资料都保持可用；
 *   - 位置与天气转简短文本；图片与软件列表只取用户显式选中条目；
 *     文件只读用户指定的单个文件、不递归；
 *   - 原始图片、原始文件、完整软件清单不上传；注入前脱敏；
 *   - 日志只记能力名与状态，不记坐标、文件名、图片内容。
 */
#ifndef DSC_CAPABILITY_H
#define DSC_CAPABILITY_H

#include <stddef.h>

#include "dsc_types.h"
#include "db.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_CAP_TIME      "time"
#define DSC_CAP_LOCATION  "location"
#define DSC_CAP_WEATHER   "weather"
#define DSC_CAP_MEDIA     "media"
#define DSC_CAP_APPS      "apps"
#define DSC_CAP_FILES     "files"

#define DSC_CAP_COUNT     6
#define DSC_CAP_FILE_MAX  (64 * 1024)  /* 单文件最多读 64KB 供注入 */

/* 授权状态。state 是库里的持久值，granted 是它的便捷形式。 */
typedef struct dsc_cap_status {
    char kind[24];
    int  granted;
    char state[16];        /* granted / denied / unset */
    char requested_at[32];
    char granted_at[32];
    char last_used_at[32];
    int  deny_count;
} dsc_cap_status;

/* 能力元数据。顺序与 DSC_CAP_COUNT 一致。 */
size_t      dsc_cap_count(void);
const char *dsc_cap_kind_at(size_t i);
const char *dsc_cap_kind_valid(const char *kind);   /* 未知返回 NULL */
const char *dsc_cap_name(const char *kind);         /* 中文名，如"位置" */
const char *dsc_cap_purpose(const char *kind);      /* 申请前展示的用途说明 */
int         dsc_cap_required(const char *kind);     /* 时间返回 1 */
int         dsc_cap_revocable(const char *kind);    /* 时间返回 0 */

/* 授权状态读写。行不存在时视为 unset。 */
int dsc_cap_status_load(dsc_db *db, const char *kind, dsc_cap_status *out);
int dsc_cap_is_granted(dsc_db *db, const char *kind);
/* 申请：写入 requested_at 并把状态置为 unset（等待用户答复），返回用途文案。 */
int dsc_cap_request(dsc_db *db, const char *kind, char *purpose, size_t purpose_cap);
int dsc_cap_grant(dsc_db *db, const char *kind, int64_t now_ms);
int dsc_cap_deny(dsc_db *db, const char *kind);   /* 记一次拒绝，主流程继续 */
int dsc_cap_revoke(dsc_db *db, const char *kind); /* 时间能力返回 DSC_E_STATE */
/* 首次启动门：时间未授予返回 0（不进主页）。 */
int dsc_cap_launch_gate(dsc_db *db);

/* ---------- 平台侧实现 ----------
 * 平台注入六个取值函数；返回 DSC_OK 表示本次确有一份可注入的值写进了 buf。
 * 返回 DSC_E_STATE 表示平台不支持或用户这次没有选中任何条目（能力已授权也不算错），
 * 其它错误按 §9.4 写错误中心。
 * 未注入 provider 时：时间用本地时钟真实现，其余一律 DSC_E_STATE。 */
typedef struct dsc_cap_provider {
    int (*time_now)(char *buf, size_t cap, void *ud);
    int (*location)(char *buf, size_t cap, void *ud);
    int (*weather)(char *buf, size_t cap, void *ud);
    int (*media_pick)(char *buf, size_t cap, void *ud);
    int (*apps_pick)(char *buf, size_t cap, void *ud);
    int (*files_pick)(char *buf, size_t cap, void *ud);
    void *ud;
} dsc_cap_provider;

void dsc_cap_set_provider(const dsc_cap_provider *p);
void dsc_cap_clear_provider(void);

/* 取一次能力值（已授权才取）。成功把简短文本写进 out，并更新 last_used_at。
 * data_root 非空时失败按 §9.4 写错误中心（只有能力名与状态，没有参数）。 */
int dsc_cap_invoke(dsc_db *db, const char *kind, const char *data_root, sb *out);

/* 注入用片段：只包含已授权且本次取到值的能力，注入前统一脱敏。
 * 无任何可注入项时 out 置空并返回 DSC_OK。 */
int dsc_cap_build_context(dsc_db *db, const char *data_root, sb *out);

/* 只读用户指定的单个文件（拒绝目录、不递归），供"本地文件"能力使用。
 * 二进制或超限只取可读文本前缀；返回 DSC_OK 时 out 里是可直接注入的文本。 */
int dsc_cap_read_text_file(dsc_db *db, const char *path, sb *out);

#ifdef __cplusplus
}
#endif

#endif /* DSC_CAPABILITY_H */
