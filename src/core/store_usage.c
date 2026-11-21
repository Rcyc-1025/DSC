/* store_usage.c */
#include "store_usage.h"

#include <string.h>

static void read_usage(aic_stmt *st, aic_usage_row *r)
{
    const char *v;
    memset(r, 0, sizeof(*r));
    if ((v = aic_stmt_text(st, 0))) aic_strlcpy_utf8(r->id, sizeof(r->id), v);
    if ((v = aic_stmt_text(st, 1))) aic_strlcpy_utf8(r->conversation_id, sizeof(r->conversation_id), v);
    if ((v = aic_stmt_text(st, 2))) aic_strlcpy_utf8(r->message_id, sizeof(r->message_id), v);
    if ((v = aic_stmt_text(st, 3))) aic_strlcpy_utf8(r->model_id, sizeof(r->model_id), v);
    r->prompt_tokens     = aic_stmt_ll(st, 4);
    r->completion_tokens = aic_stmt_ll(st, 5);
    r->total_tokens      = aic_stmt_ll(st, 6);
    r->cost_micros       = aic_stmt_ll(st, 7);
    if ((v = aic_stmt_text(st, 8))) aic_strlcpy_utf8(r->created_at, sizeof(r->created_at), v);
}

int aic_usage_append(aic_db *db, const char *conv_id, const char *msg_id, const char *model_id,
                     long long prompt_tokens, long long completion_tokens, long long cost_micros)
{
    if (!db || !model_id) return AIC_E_INVAL;
    char id[40];
    aic_uuid4(id);
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO usage_ledger (id, conversation_id, message_id, model_id, prompt_tokens, "
        "completion_tokens, total_tokens, cost_micros, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, conv_id);
    aic_stmt_bind_text(st, 3, msg_id);
    aic_stmt_bind_text(st, 4, model_id);
    aic_stmt_bind_ll(st, 5, prompt_tokens);
    aic_stmt_bind_ll(st, 6, completion_tokens);
    aic_stmt_bind_ll(st, 7, prompt_tokens + completion_tokens);
    aic_stmt_bind_ll(st, 8, cost_micros);
    aic_stmt_bind_text(st, 9, now);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_usage_summary_get(aic_db *db, int64_t since_ms, aic_usage_summary *out)
{
    if (!db || !out) return AIC_E_INVAL;
    memset(out, 0, sizeof(*out));
    aic_stmt *st;
    if (since_ms > 0) {
        char since[32];
        aic_ms_to_iso8601(since_ms, since);
        st = aic_db_prepare(db,
            "SELECT COUNT(*), IFNULL(SUM(prompt_tokens),0), IFNULL(SUM(completion_tokens),0), "
            "IFNULL(SUM(total_tokens),0), IFNULL(SUM(cost_micros),0) "
            "FROM usage_ledger WHERE created_at >= ?1;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, since);
    } else {
        st = aic_db_prepare(db,
            "SELECT COUNT(*), IFNULL(SUM(prompt_tokens),0), IFNULL(SUM(completion_tokens),0), "
            "IFNULL(SUM(total_tokens),0), IFNULL(SUM(cost_micros),0) FROM usage_ledger;");
        if (!st) return AIC_E_DB;
    }
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        out->requests          = aic_stmt_ll(st, 0);
        out->prompt_tokens     = aic_stmt_ll(st, 1);
        out->completion_tokens = aic_stmt_ll(st, 2);
        out->total_tokens      = aic_stmt_ll(st, 3);
        out->cost_micros       = aic_stmt_ll(st, 4);
    }
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : AIC_E_DB;
}

int aic_usage_recent(aic_db *db, int limit, aic_usage_row **out, size_t *out_n)
{
    if (!db || !out || !out_n) return AIC_E_INVAL;
    if (limit <= 0) limit = 20;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT id, IFNULL(conversation_id,''), IFNULL(message_id,''), model_id, prompt_tokens, "
        "completion_tokens, total_tokens, cost_micros, created_at FROM usage_ledger "
        "ORDER BY created_at DESC LIMIT ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_ll(st, 1, limit);
    size_t cap = 16, n = 0;
    aic_usage_row *arr = (aic_usage_row *)aic_xcalloc(cap, sizeof(aic_usage_row));
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (aic_usage_row *)aic_xrealloc(arr, cap * sizeof(aic_usage_row));
        }
        read_usage(st, &arr[n++]);
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) {
        aic_xfree(arr);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

void aic_usage_free_rows(aic_usage_row *arr, size_t n)
{
    (void)n;
    aic_xfree(arr);
}
