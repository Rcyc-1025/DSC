/* error_store.c —— 错误中心的落库、查询、脱敏与诊断包（§9.4 / FR-34）。 */
#include "error_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "llm.h"
#include "store_setting.h"

/* ---------- 分类与动作 ---------- */

const char *aic_error_category_valid(const char *c)
{
    static const char *all[] = {
        AIC_ERR_NETWORK, AIC_ERR_QUOTA, AIC_ERR_RATE_LIMIT, AIC_ERR_PARSE,
        AIC_ERR_CREDENTIAL, AIC_ERR_CAPABILITY, AIC_ERR_STORAGE, AIC_ERR_OTHER
    };
    if (!c) return AIC_ERR_OTHER;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcmp(c, all[i]) == 0) return all[i];
    }
    return AIC_ERR_OTHER;
}

const char *aic_error_category_text(const char *c)
{
    const char *v = aic_error_category_valid(c);
    if (strcmp(v, AIC_ERR_NETWORK) == 0) return "网络";
    if (strcmp(v, AIC_ERR_QUOTA) == 0) return "额度";
    if (strcmp(v, AIC_ERR_RATE_LIMIT) == 0) return "限流";
    if (strcmp(v, AIC_ERR_PARSE) == 0) return "解析";
    if (strcmp(v, AIC_ERR_CREDENTIAL) == 0) return "凭证";
    if (strcmp(v, AIC_ERR_CAPABILITY) == 0) return "设备能力";
    if (strcmp(v, AIC_ERR_STORAGE) == 0) return "存储";
    return "其它";
}

const char *aic_error_category_from_llm(int llm_err)
{
    switch (llm_err) {
    case AIC_LLM_E_NETWORK:
    case AIC_LLM_E_SERVER:   return AIC_ERR_NETWORK;
    case AIC_LLM_E_AUTH:     return AIC_ERR_CREDENTIAL;
    case AIC_LLM_E_RATE:     return AIC_ERR_RATE_LIMIT;
    case AIC_LLM_E_PARSE:    return AIC_ERR_PARSE;
    case AIC_LLM_E_BALANCE:  return AIC_ERR_QUOTA;
    default:                 return AIC_ERR_OTHER;
    }
}

const char *aic_error_code_from_llm(int llm_err)
{
    switch (llm_err) {
    case AIC_LLM_E_NETWORK:  return "llm.network";
    case AIC_LLM_E_AUTH:     return "llm.auth";
    case AIC_LLM_E_RATE:     return "llm.rate_limit";
    case AIC_LLM_E_PARSE:    return "llm.parse";
    case AIC_LLM_E_SERVER:   return "llm.server_error";
    case AIC_LLM_E_BADREQ:   return "llm.bad_request";
    case AIC_LLM_E_NOMODEL:  return "llm.model_missing";
    case AIC_LLM_E_BALANCE:  return "llm.balance_low";
    case AIC_LLM_E_CANCEL:   return "llm.cancelled";
    default:                 return "llm.unknown";
    }
}

aic_error_action aic_error_action_for(const char *category)
{
    const char *c = aic_error_category_valid(category);
    if (strcmp(c, AIC_ERR_NETWORK) == 0)    return AIC_ERR_ACT_RETRY;
    if (strcmp(c, AIC_ERR_QUOTA) == 0)      return AIC_ERR_ACT_BALANCE;
    if (strcmp(c, AIC_ERR_CREDENTIAL) == 0) return AIC_ERR_ACT_SETTINGS;
    if (strcmp(c, AIC_ERR_RATE_LIMIT) == 0) return AIC_ERR_ACT_SETTINGS;
    if (strcmp(c, AIC_ERR_CAPABILITY) == 0) return AIC_ERR_ACT_CAPABILITY;
    if (strcmp(c, AIC_ERR_PARSE) == 0)      return AIC_ERR_ACT_DETAIL;
    if (strcmp(c, AIC_ERR_STORAGE) == 0)    return AIC_ERR_ACT_DETAIL;
    return AIC_ERR_ACT_NONE;
}

const char *aic_error_action_text(aic_error_action act)
{
    switch (act) {
    case AIC_ERR_ACT_RETRY:      return "重试";
    case AIC_ERR_ACT_BALANCE:    return "看余额";
    case AIC_ERR_ACT_SETTINGS:   return "去设置";
    case AIC_ERR_ACT_CAPABILITY: return "去权限";
    case AIC_ERR_ACT_DETAIL:     return "看诊断";
    default:                     return "";
    }
}

/* ---------- 脱敏 ---------- */

static int ci_prefix(const char *s, const char *pfx)
{
    while (*pfx) {
        if (!*s) return 0;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pfx)) return 0;
        s++; pfx++;
    }
    return 1;
}

static int is_token_char(int c)
{
    return isalnum(c) || c == '_' || c == '-' || c == '.';
}

static int is_hex_char(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_email_local(int c)
{
    return isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

static void sb_truncate(sb *b, size_t n)
{
    if (!b) return;
    if (b->len > n) { b->len = n; if (b->data) b->data[n] = '\0'; }
}

/* 无引号键值脱敏（日志行、非严格 JSON）：api_key=xxx / token: xxx。
 * 返回被消费的字节数，0 表示不匹配。 */
static size_t redact_kv(const char *p, sb *out, int *changed)
{
    static const char *names[] = { "api_key", "authorization", "access_token", "secret" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *nm = names[i];
        if (!ci_prefix(p, nm)) continue;
        const char *q = p + strlen(nm);
        if (*q == '"') continue;                 /* 严格 JSON 交给上一条规则 */
        const char *r = q;
        while (*r == ' ' || *r == '\t') r++;
        char sep = 0;
        if (*r == '=' || *r == ':') { sep = *r; r++; } else continue;
        while (*r == ' ' || *r == '\t') r++;
        int quoted = 0;
        if (*r == '"') { quoted = 1; r++; }
        size_t n = 0;
        if (quoted) {
            while (r[n] && r[n] != '"') n++;
            if (r[n] != '"') return 0;
        } else {
            while (r[n] && r[n] != '\n' && r[n] != '\r' && r[n] != ',' && r[n] != ';' &&
                   r[n] != '"' && r[n] != '\'' && r[n] != '&' && r[n] != '}')
                n++;
            while (n > 0 && (r[n - 1] == ' ' || r[n - 1] == '\t')) n--;
        }
        if (n < 6) return 0;
        sb_append(out, nm);
        sb_appendc(out, sep);
        if (quoted) sb_append(out, "\"***\""); else sb_append(out, "***");
        if (changed) *changed = 1;
        return (size_t)((r + n + (quoted ? 1 : 0)) - p);
    }
    return 0;
}

int aic_error_sanitize(const char *in, sb *out)
{
    if (!out) return 0;
    if (!in) return 0;
    sb_reset(out);
    int changed = 0;
    const char *p = in;
    while (*p) {
        /* 1) sk- 开头的密钥。 */
        if (p[0] == 's' && p[1] == 'k' && p[2] == '-') {
            const char *q = p + 3;
            size_t n = 0;
            while (is_token_char((unsigned char)q[n])) n++;
            if (n >= 8) {
                sb_append(out, "sk-***");
                p = q + n;
                changed = 1;
                continue;
            }
        }
        /* 2) Bearer 令牌。 */
        if (ci_prefix(p, "bearer ")) {
            const char *q = p + 7;
            size_t n = 0;
            while (is_token_char((unsigned char)q[n])) n++;
            if (n >= 8) {
                sb_append(out, "Bearer ***");
                p = q + n;
                changed = 1;
                continue;
            }
        }
        /* 3) JSON 里的密钥字段： "api_key":"..." / "authorization":"..." / "token":"..." */
        if (*p == '"' && (ci_prefix(p, "\"api_key\"") || ci_prefix(p, "\"authorization\"") ||
                          ci_prefix(p, "\"access_token\"") || ci_prefix(p, "\"secret\""))) {
            const char *q = p + 1;
            size_t klen = 0;
            while (q[klen] && q[klen] != '"') klen++;
            if (q[klen] == '"') {
                const char *r = q + klen + 1;
                while (*r == ' ' || *r == ':' || *r == '\t') r++;
                if (*r == '"') {
                    const char *v = r + 1;
                    size_t vlen = 0;
                    while (v[vlen] && v[vlen] != '"') vlen++;
                    if (v[vlen] == '"') {
                        sb_append(out, "\"");
                        sb_appendn(out, p + 1, klen);
                        sb_append(out, "\":\"***\"");
                        p = v + vlen + 1;
                        changed = 1;
                        continue;
                    }
                }
            }
        }
        /* 3b) 无引号键值：api_key=xxx / authorization: xxx。 */
        {
            size_t used = redact_kv(p, out, &changed);
            if (used) { p += used; continue; }
        }
        /* 4) 32 位以上十六进制串（会话密钥、哈希）。 */
        if (is_hex_char((unsigned char)*p)) {
            size_t n = 0;
            while (is_hex_char((unsigned char)p[n])) n++;
            if (n >= 32) {
                sb_append(out, "***");
                p += n;
                changed = 1;
                continue;
            }
        }
        /* 5) 邮箱：把 @ 前面的本地部分也隐去。 */
        if (*p == '@' && out->len > 0) {
            size_t keep = out->len;
            while (keep > 0 && is_email_local((unsigned char)out->data[keep - 1])) keep--;
            if (keep < out->len) {
                sb_truncate(out, keep);
                sb_append(out, "***@");
                p++;
                changed = 1;
                while (*p && (isalnum((unsigned char)*p) || *p == '.' || *p == '-')) p++;
                continue;
            }
        }
        sb_appendc(out, *p);
        p++;
    }
    return changed;
}

/* ---------- 写入 ---------- */

static int bump_unread(aic_db *db, int delta)
{
    int cur = aic_setting_get_int(db, AIC_SET_ERROR_UNREAD, 0);
    cur += delta;
    if (cur < 0) cur = 0;
    return aic_setting_set_int(db, AIC_SET_ERROR_UNREAD, cur);
}

int aic_error_record(aic_db *db, const char *data_root, const char *category, const char *code,
                     const char *message, const char *conversation_id, const char *session_id,
                     const char *detail_raw, aic_error_entry *out)
{
    if (!db) return AIC_E_INVAL;
    aic_error_entry rec;
    memset(&rec, 0, sizeof(rec));
    aic_uuid4(rec.id);
    snprintf(rec.category, sizeof(rec.category), "%s", aic_error_category_valid(category));
    /* 错误码与描述都过一遍脱敏，避免把密钥带进错误中心。 */
    sb clean;
    sb_init(&clean);
    aic_error_sanitize(code ? code : "", &clean);
    aic_strlcpy_utf8(rec.code, sizeof(rec.code), sb_cstr(&clean));
    sb_reset(&clean);
    aic_error_sanitize(message ? message : "", &clean);
    aic_strlcpy_utf8(rec.message, sizeof(rec.message), sb_cstr(&clean));
    sb_free(&clean);
    if (conversation_id) aic_strlcpy_utf8(rec.conversation_id, sizeof(rec.conversation_id), conversation_id);
    if (session_id) aic_strlcpy_utf8(rec.session_id, sizeof(rec.session_id), session_id);
    aic_now_iso8601(rec.occurred_at);

    /* 明细：脱敏后写 errors/<id>.json，库里只留相对路径。 */
    if (data_root && data_root[0] && detail_raw && detail_raw[0]) {
        sb det;
        sb_init(&det);
        aic_error_sanitize(detail_raw, &det);
        if (det.len > AIC_ERR_DETAIL_CAP) {
            det.len = AIC_ERR_DETAIL_CAP;
            if (det.data) det.data[det.len] = '\0';
        }
        char dir[600];
        aic_path_join(dir, sizeof(dir), data_root, "errors");
        if (aic_mkdir_p(dir) == AIC_OK) {
            char file[700], rel[600];
            snprintf(rel, sizeof(rel), "errors/%s.json", rec.id);
            aic_path_join(file, sizeof(file), data_root, rel);
            sb json;
            sb_init(&json);
            jw w;
            jw_init(&w, &json, 1);
            jw_obj_begin(&w, NULL);
            jw_put_str(&w, "id", rec.id);
            jw_put_str(&w, "category", rec.category);
            jw_put_str(&w, "code", rec.code);
            jw_put_str(&w, "message", rec.message);
            jw_put_str(&w, "conversation_id", rec.conversation_id);
            jw_put_str(&w, "occurred_at", rec.occurred_at);
            jw_put_str(&w, "detail", sb_cstr(&det));
            jw_obj_end(&w);
            int wrc = aic_file_write_all(file, sb_cstr(&json), json.len);
            sb_free(&json);
            if (wrc == AIC_OK) aic_strlcpy_utf8(rec.detail_path, sizeof(rec.detail_path), rel);
        }
        sb_free(&det);
    }

    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO error_record (id, category, code, message, conversation_id, session_id,"
        " occurred_at, detail_path, acknowledged) VALUES (?,?,?,?,?,?,?,?,0);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, rec.id);
    aic_stmt_bind_text(st, 2, rec.category);
    aic_stmt_bind_text(st, 3, rec.code);
    aic_stmt_bind_text(st, 4, rec.message);
    aic_stmt_bind_text(st, 5, rec.conversation_id);
    aic_stmt_bind_text(st, 6, rec.session_id);
    aic_stmt_bind_text(st, 7, rec.occurred_at);
    aic_stmt_bind_text(st, 8, rec.detail_path);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    bump_unread(db, 1);
    if (out) *out = rec;
    return AIC_OK;
}

/* ---------- 查询 ---------- */

static void row_to_record(aic_stmt *st, aic_error_entry *r)
{
    memset(r, 0, sizeof(*r));
    aic_strlcpy_utf8(r->id, sizeof(r->id), aic_stmt_text(st, 0));
    aic_strlcpy_utf8(r->category, sizeof(r->category), aic_stmt_text(st, 1));
    aic_strlcpy_utf8(r->code, sizeof(r->code), aic_stmt_text(st, 2));
    aic_strlcpy_utf8(r->message, sizeof(r->message), aic_stmt_text(st, 3));
    aic_strlcpy_utf8(r->conversation_id, sizeof(r->conversation_id), aic_stmt_text(st, 4));
    aic_strlcpy_utf8(r->session_id, sizeof(r->session_id), aic_stmt_text(st, 5));
    aic_strlcpy_utf8(r->occurred_at, sizeof(r->occurred_at), aic_stmt_text(st, 6));
    aic_strlcpy_utf8(r->detail_path, sizeof(r->detail_path), aic_stmt_text(st, 7));
    r->acknowledged = aic_stmt_int(st, 8);
}

int aic_error_query(aic_db *db, const aic_error_filter *f, aic_error_entry *out, int cap, int *out_n)
{
    if (out_n) *out_n = 0;
    if (!db || !out || cap <= 0) return AIC_E_INVAL;
    sb sql;
    sb_init(&sql);
    sb_append(&sql, "SELECT id, category, code, message, conversation_id, session_id, occurred_at,"
                    " detail_path, acknowledged FROM error_record WHERE 1=1");
    if (f && f->category && f->category[0]) sb_appendf(&sql, " AND category='%s'",
                                                       aic_error_category_valid(f->category));
    if (f && f->since_ms > 0) {
        char since[32];
        aic_ms_to_iso8601(f->since_ms, since);
        sb_appendf(&sql, " AND occurred_at >= '%s'", since);
    }
    if (f && f->only_unack) sb_append(&sql, " AND acknowledged = 0");
    int limit = (f && f->limit > 0) ? f->limit : 200;
    if (limit > AIC_ERR_QUERY_MAX) limit = AIC_ERR_QUERY_MAX;
    sb_appendf(&sql, " ORDER BY occurred_at DESC, id DESC LIMIT %d;", limit);

    aic_stmt *st = aic_db_prepare(db, sb_cstr(&sql));
    sb_free(&sql);
    if (!st) return AIC_E_DB;
    int n = 0;
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n < cap) row_to_record(st, &out[n]);
        n++;
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    if (out_n) *out_n = n;
    return AIC_OK;
}

int aic_error_count(aic_db *db, int only_unack)
{
    if (!db) return 0;
    aic_stmt *st = aic_db_prepare(db, only_unack
        ? "SELECT COUNT(*) FROM error_record WHERE acknowledged = 0;"
        : "SELECT COUNT(*) FROM error_record;");
    if (!st) return 0;
    int n = 0;
    if (aic_stmt_step(st) == AIC_ROW) n = aic_stmt_int(st, 0);
    aic_stmt_finalize(st);
    return n;
}

int aic_error_unread(aic_db *db)
{
    if (!db) return 0;
    int n = aic_setting_get_int(db, AIC_SET_ERROR_UNREAD, 0);
    if (n < 0) n = 0;
    return n;
}

int aic_error_ack_all(aic_db *db)
{
    if (!db) return AIC_E_INVAL;
    int rc = aic_db_exec(db, "UPDATE error_record SET acknowledged = 1 WHERE acknowledged = 0;");
    if (rc != AIC_OK) return rc;
    return aic_setting_set_int(db, AIC_SET_ERROR_UNREAD, 0);
}

int aic_error_clear(aic_db *db, int64_t before_ms, int *out_removed)
{
    if (out_removed) *out_removed = 0;
    if (!db) return AIC_E_INVAL;
    int before = aic_error_count(db, 0);
    int rc;
    if (before_ms > 0) {
        char iso[32];
        aic_ms_to_iso8601(before_ms, iso);
        aic_stmt *st = aic_db_prepare(db, "DELETE FROM error_record WHERE occurred_at < ?;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, iso);
        rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        if (rc != AIC_DONE) return AIC_E_DB;
    } else {
        rc = aic_db_exec(db, "DELETE FROM error_record;");
        if (rc != AIC_OK) return rc;
    }
    int after = aic_error_count(db, 0);
    if (out_removed) *out_removed = before - after;
    return aic_setting_set_int(db, AIC_SET_ERROR_UNREAD, aic_error_count(db, 1));
}

int aic_error_retention_days(aic_db *db)
{
    return aic_setting_get_int(db, AIC_SET_ERROR_RETENTION, 30);
}

int aic_error_set_retention_days(aic_db *db, int days)
{
    if (days < 0) days = 0;
    if (days > 3650) days = 3650;
    return aic_setting_set_int(db, AIC_SET_ERROR_RETENTION, days);
}

int aic_error_cleanup(aic_db *db, int *out_removed)
{
    if (out_removed) *out_removed = 0;
    if (!db) return AIC_E_INVAL;
    int days = aic_error_retention_days(db);
    if (days <= 0) return AIC_OK;
    int64_t cutoff = aic_now_ms() - (int64_t)days * 86400000LL;
    return aic_error_clear(db, cutoff, out_removed);
}

/* ---------- 诊断包 ---------- */

int aic_error_export_diagnostic(aic_db *db, const char *path, char *err, size_t errlen)
{
    if (!db || !path || !path[0]) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    aic_error_entry recs[200];
    int n = 0;
    aic_error_filter f;
    memset(&f, 0, sizeof(f));
    f.limit = 200;
    int rc = aic_error_query(db, &f, recs, 200, &n);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "读取错误记录失败");
        return rc;
    }

    sb json;
    sb_init(&json);
    jw w;
    jw_init(&w, &json, 1);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "format", "sai_diagnostic");
    jw_put_int(&w, "schema_version", 1);
    char now[32];
    aic_now_iso8601(now);
    jw_put_str(&w, "exported_at", now);
    jw_put_int(&w, "schema_app", AIC_SCHEMA_LATEST());
    jw_put_int(&w, "error_total", aic_error_count(db, 0));
    jw_put_int(&w, "error_unread", aic_error_count(db, 1));

    /* 设置快照只带与排障相关的键，且都已脱敏（不含 API Key 本体）。 */
    jw_obj_begin(&w, "settings");
    jw_put_str(&w, "theme", aic_setting_get_str(db, AIC_SET_THEME, "system", (char[32]){0}, 32));
    jw_put_str(&w, "default_mode", aic_setting_get_str(db, AIC_SET_DEFAULT_MODE, "qa", (char[32]){0}, 32));
    jw_put_str(&w, "model_default", aic_setting_get_str(db, AIC_SET_MODEL_DEFAULT, AIC_MODEL_FLASH, (char[64]){0}, 64));
    jw_put_int(&w, "has_api_key", aic_setting_get_int(db, AIC_SET_CRED_HAS_KEY, 0));
    jw_put_int(&w, "error_retention_days", aic_error_retention_days(db));
    jw_obj_end(&w);

    jw_arr_begin(&w, "errors");
    for (int i = 0; i < n && i < 200; i++) {
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "id", recs[i].id);
        jw_put_str(&w, "category", recs[i].category);
        jw_put_str(&w, "code", recs[i].code);
        jw_put_str(&w, "message", recs[i].message);
        jw_put_str(&w, "conversation_id", recs[i].conversation_id);
        jw_put_str(&w, "occurred_at", recs[i].occurred_at);
        jw_put_str(&w, "detail_path", recs[i].detail_path);
        jw_put_int(&w, "acknowledged", recs[i].acknowledged);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);

    rc = aic_file_write_all(path, sb_cstr(&json), json.len);
    sb_free(&json);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "写诊断包失败");
        return rc;
    }
    return AIC_OK;
}
