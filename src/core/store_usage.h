/* store_usage.h —— 用量账本（FR-11：余额与用量查询）。 */
#ifndef AIC_STORE_USAGE_H
#define AIC_STORE_USAGE_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aic_usage_row {
    char      id[40];
    char      conversation_id[40];
    char      message_id[40];
    char      model_id[64];
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    long long cost_micros;   /* 微元，避免浮点误差 */
    char      created_at[32];
} aic_usage_row;

typedef struct aic_usage_summary {
    long long requests;
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    long long cost_micros;
} aic_usage_summary;

int aic_usage_append(aic_db *db, const char *conv_id, const char *msg_id, const char *model_id,
                     long long prompt_tokens, long long completion_tokens, long long cost_micros);
/* 汇总：since_ms<=0 表示全部；否则只统计 created_at >= since_ms 的记录。 */
int aic_usage_summary_get(aic_db *db, int64_t since_ms, aic_usage_summary *out);
int aic_usage_recent(aic_db *db, int limit, aic_usage_row **out, size_t *out_n);
void aic_usage_free_rows(aic_usage_row *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
