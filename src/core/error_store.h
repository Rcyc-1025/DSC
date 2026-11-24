/* error_store.h —— 错误中心（技术设计 §9.4 / FR-34）。
 *
 * 三条硬约束（都写进实现与验收）：
 *   1. 错误不进消息表、不进上下文、不进聊天框：这里单独一张 error_record 表，
 *      正文只写错误中心，聊天区最多出现一条非侵入状态条；
 *   2. 原始响应必须脱敏后才落盘：明细写 errors/<id>.json，先过 dsc_error_sanitize；
 *   3. 日志与导出只记能力名、错误码与时间，不记坐标、文件名、图片内容（见 §18）。
 *
 * 分类（§5.2）：network / quota / rate_limit / parse / credential / capability / storage / other。
 * 每条记录带"下一步动作"提示：额度不足给余额入口、网络中断给重试入口。
 */
#ifndef DSC_ERROR_STORE_H
#define DSC_ERROR_STORE_H

#include "dsc_types.h"
#include "db.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_ERR_NETWORK     "network"
#define DSC_ERR_QUOTA       "quota"
#define DSC_ERR_RATE_LIMIT  "rate_limit"
#define DSC_ERR_PARSE       "parse"
#define DSC_ERR_CREDENTIAL  "credential"
#define DSC_ERR_CAPABILITY  "capability"
#define DSC_ERR_STORAGE     "storage"
#define DSC_ERR_OTHER       "other"

#define DSC_ERR_MSG_CAP     512
#define DSC_ERR_DETAIL_CAP  (8 * 1024)
#define DSC_ERR_QUERY_MAX   500

/* 错误中心里的"下一步动作"（界面据此决定放哪个入口）。 */
typedef enum dsc_error_action {
    DSC_ERR_ACT_NONE = 0,
    DSC_ERR_ACT_RETRY,      /* 网络中断：沿用原草稿重试，不产生新消息 */
    DSC_ERR_ACT_BALANCE,    /* 额度不足：跳余额与用量 */
    DSC_ERR_ACT_SETTINGS,   /* 凭证失效/限流：跳设置 */
    DSC_ERR_ACT_CAPABILITY, /* 能力被拒：跳设备权限 */
    DSC_ERR_ACT_DETAIL      /* 解析/存储：看诊断包 */
} dsc_error_action;

typedef struct dsc_error_entry {
    char id[40];
    char category[24];
    char code[64];            /* 脱敏后的错误码，如 llm.http_401 */
    char message[DSC_ERR_MSG_CAP];
    char conversation_id[40]; /* 关联会话，可为空 */
    char session_id[40];      /* 运行会话，可为空 */
    char occurred_at[32];
    char detail_path[512];    /* errors/<id>.json 相对路径，可为空 */
    int  acknowledged;
} dsc_error_entry;

typedef struct dsc_error_filter {
    const char *category;  /* NULL 或空表示全部 */
    int64_t     since_ms;  /* 0 表示不限 */
    int         only_unack;
    int         limit;     /* <=0 用默认值 */
} dsc_error_filter;

/* 写一条错误。data_root 为空时只写库、不写明细文件（测试与无数据目录场景）。
 * detail_raw 可为空；非空时脱敏后写入 errors/<id>.json 并记 detail_path。
 * 入参非法返回 DSC_E_INVAL；库里记录失败返回 DSC_E_DB。 */
int dsc_error_record(dsc_db *db, const char *data_root, const char *category, const char *code,
                     const char *message, const char *conversation_id, const char *session_id,
                     const char *detail_raw, dsc_error_entry *out);

/* 查询：按类别与时间过滤，新的在前。out_n 返回实际写入条数（可能大于 cap，按 cap 截断）。 */
int dsc_error_query(dsc_db *db, const dsc_error_filter *f, dsc_error_entry *out, int cap, int *out_n);
int dsc_error_count(dsc_db *db, int only_unack);
int dsc_error_unread(dsc_db *db);
int dsc_error_ack_all(dsc_db *db);
/* 清除：before_ms=0 清全部；返回删除条数。 */
int dsc_error_clear(dsc_db *db, int64_t before_ms, int *out_removed);
int dsc_error_retention_days(dsc_db *db);
int dsc_error_set_retention_days(dsc_db *db, int days);
/* 按保留期清理（0 表示不清理），返回删除条数。 */
int dsc_error_cleanup(dsc_db *db, int *out_removed);

/* 诊断包：一条 JSON（版本、时间、设置快照、错误列表），已脱敏，可直接交出去。 */
int dsc_error_export_diagnostic(dsc_db *db, const char *path, char *err, size_t errlen);

/* 分类映射与文案。 */
const char *dsc_error_category_valid(const char *category);
const char *dsc_error_category_text(const char *category);
const char *dsc_error_category_from_llm(int llm_err);
const char *dsc_error_code_from_llm(int llm_err);
dsc_error_action dsc_error_action_for(const char *category);
const char *dsc_error_action_text(dsc_error_action act);

/* 脱敏：把 sk- 密钥、Bearer 令牌、api_key 字段、长十六进制串替换掉。
 * 返回 1 表示确有替换，0 表示原文未含敏感内容。 */
int dsc_error_sanitize(const char *in, sb *out);

#ifdef __cplusplus
}
#endif

#endif /* DSC_ERROR_STORE_H */
