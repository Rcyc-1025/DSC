/* store_usage.h —— 用量账本（FR-11：余额与用量查询）。 */
#ifndef DSC_STORE_USAGE_H
#define DSC_STORE_USAGE_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsc_usage_row {
    char      id[40];
    char      conversation_id[40];
    char      message_id[40];
    char      model_id[64];
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    long long cost_micros;   /* 微元，避免浮点误差 */
    char      created_at[32];
} dsc_usage_row;

typedef struct dsc_usage_summary {
    long long requests;
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    long long cost_micros;
} dsc_usage_summary;

int dsc_usage_append(dsc_db *db, const char *conv_id, const char *msg_id, const char *model_id,
                     long long prompt_tokens, long long completion_tokens, long long cost_micros);
/* 汇总：since_ms<=0 表示全部；否则只统计 created_at >= since_ms 的记录。 */
int dsc_usage_summary_get(dsc_db *db, int64_t since_ms, dsc_usage_summary *out);
int dsc_usage_recent(dsc_db *db, int limit, dsc_usage_row **out, size_t *out_n);
void dsc_usage_free_rows(dsc_usage_row *arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif
