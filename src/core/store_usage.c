/* store_usage.c */
#include "store_usage.h"

#include <string.h>

static void read_usage(dsc_stmt *st, dsc_usage_row *r)
{
    const char *v;
    memset(r, 0, sizeof(*r));
    if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(r->id, sizeof(r->id), v);
    if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(r->conversation_id, sizeof(r->conversation_id), v);
    if ((v = dsc_stmt_text(st, 2))) dsc_strlcpy_utf8(r->message_id, sizeof(r->message_id), v);
    if ((v = dsc_stmt_text(st, 3))) dsc_strlcpy_utf8(r->model_id, sizeof(r->model_id), v);
    r->prompt_tokens     = dsc_stmt_ll(st, 4);
    r->completion_tokens = dsc_stmt_ll(st, 5);
    r->total_tokens      = dsc_stmt_ll(st, 6);
    r->cost_micros       = dsc_stmt_ll(st, 7);
    if ((v = dsc_stmt_text(st, 8))) dsc_strlcpy_utf8(r->created_at, sizeof(r->created_at), v);
}

int dsc_usage_append(dsc_db *db, const char *conv_id, const char *msg_id, const char *model_id,
                     long long prompt_tokens, long long completion_tokens, long long cost_micros)
{
    if (!db || !model_id) return DSC_E_INVAL;
    char id[40];
    dsc_uuid4(id);
    char now[32];
    dsc_now_iso8601(now);
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO usage_ledger (id, conversation_id, message_id, model_id, prompt_tokens, "
        "completion_tokens, total_tokens, cost_micros, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    dsc_stmt_bind_text(st, 2, conv_id);
    dsc_stmt_bind_text(st, 3, msg_id);
    dsc_stmt_bind_text(st, 4, model_id);
    dsc_stmt_bind_ll(st, 5, prompt_tokens);
    dsc_stmt_bind_ll(st, 6, completion_tokens);
    dsc_stmt_bind_ll(st, 7, prompt_tokens + completion_tokens);
    dsc_stmt_bind_ll(st, 8, cost_micros);
    dsc_stmt_bind_text(st, 9, now);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_usage_summary_get(dsc_db *db, int64_t since_ms, dsc_usage_summary *out)
{
    if (!db || !out) return DSC_E_INVAL;
    memset(out, 0, sizeof(*out));
    dsc_stmt *st;
    if (since_ms > 0) {
        char since[32];
        dsc_ms_to_iso8601(since_ms, since);
        st = dsc_db_prepare(db,
            "SELECT COUNT(*), IFNULL(SUM(prompt_tokens),0), IFNULL(SUM(completion_tokens),0), "
            "IFNULL(SUM(total_tokens),0), IFNULL(SUM(cost_micros),0) "
            "FROM usage_ledger WHERE created_at >= ?1;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, since);
    } else {
        st = dsc_db_prepare(db,
            "SELECT COUNT(*), IFNULL(SUM(prompt_tokens),0), IFNULL(SUM(completion_tokens),0), "
            "IFNULL(SUM(total_tokens),0), IFNULL(SUM(cost_micros),0) FROM usage_ledger;");
        if (!st) return DSC_E_DB;
    }
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) {
        out->requests          = dsc_stmt_ll(st, 0);
        out->prompt_tokens     = dsc_stmt_ll(st, 1);
        out->completion_tokens = dsc_stmt_ll(st, 2);
        out->total_tokens      = dsc_stmt_ll(st, 3);
        out->cost_micros       = dsc_stmt_ll(st, 4);
    }
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : DSC_E_DB;
}

int dsc_usage_recent(dsc_db *db, int limit, dsc_usage_row **out, size_t *out_n)
{
    if (!db || !out || !out_n) return DSC_E_INVAL;
    if (limit <= 0) limit = 20;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT id, IFNULL(conversation_id,''), IFNULL(message_id,''), model_id, prompt_tokens, "
        "completion_tokens, total_tokens, cost_micros, created_at FROM usage_ledger "
        "ORDER BY created_at DESC LIMIT ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_ll(st, 1, limit);
    size_t cap = 16, n = 0;
    dsc_usage_row *arr = (dsc_usage_row *)dsc_xcalloc(cap, sizeof(dsc_usage_row));
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = (dsc_usage_row *)dsc_xrealloc(arr, cap * sizeof(dsc_usage_row));
        }
        read_usage(st, &arr[n++]);
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) {
        dsc_xfree(arr);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

void dsc_usage_free_rows(dsc_usage_row *arr, size_t n)
{
    (void)n;
    dsc_xfree(arr);
}
