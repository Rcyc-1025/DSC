/* pairing.c —— 双端配对实现（技术设计 §16.2，FR-26）。 */
#include "pairing.h"
#include "credential.h"
#include "store_setting.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

struct dsc_pairing {
    dsc_db       *db;
    dsc_pair_state state;
    char          local_code[DSC_PAIR_CODE_LEN + 1];
    char          peer_code[DSC_PAIR_CODE_LEN + 1];
    long long     expires_ms;
    int           fails;
};

const char *dsc_pair_state_name(dsc_pair_state s)
{
    switch (s) {
    case DSC_PAIR_IDLE:       return "IDLE";
    case DSC_PAIR_CODE_READY: return "CODE_READY";
    case DSC_PAIR_VERIFIED:   return "VERIFIED";
    case DSC_PAIR_FAILED:     return "FAILED";
    default:                  return "UNKNOWN";
    }
}

int dsc_pairing_create(dsc_db *db, dsc_pairing **out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_pairing *p = (dsc_pairing *)dsc_xcalloc(1, sizeof(dsc_pairing));
    p->db = db;
    p->state = DSC_PAIR_IDLE;
    *out = p;
    return DSC_OK;
}

void dsc_pairing_destroy(dsc_pairing *p)
{
    if (!p) return;
    dsc_secure_zero(p->local_code, sizeof(p->local_code));
    dsc_secure_zero(p->peer_code, sizeof(p->peer_code));
    dsc_xfree(p);
}

dsc_pair_state dsc_pairing_state(const dsc_pairing *p) { return p ? p->state : DSC_PAIR_IDLE; }
const char *dsc_pairing_code(const dsc_pairing *p) { return p ? p->local_code : ""; }
long long dsc_pairing_expires_at(const dsc_pairing *p) { return p ? p->expires_ms : 0; }
int dsc_pairing_fails(const dsc_pairing *p) { return p ? p->fails : 0; }

int dsc_pair_code_valid(const char *code)
{
    if (!code) return 0;
    size_t n = strlen(code);
    if (n != DSC_PAIR_CODE_LEN) return 0;
    for (size_t i = 0; i < n; i++) {
        if (code[i] < '0' || code[i] > '9') return 0;
    }
    int v = 0;
    if (dsc_parse_int(code, &v) != DSC_OK) return 0;
    return v >= DSC_PAIR_CODE_MIN && v <= DSC_PAIR_CODE_MAX;
}

static int make_code(char out[DSC_PAIR_CODE_LEN + 1])
{
    unsigned char raw[4];
    if (dsc_random_bytes(raw, sizeof(raw)) != DSC_OK) return DSC_E_IO;
    unsigned int v = ((unsigned int)raw[0] << 24) | ((unsigned int)raw[1] << 16) |
                     ((unsigned int)raw[2] << 8) | (unsigned int)raw[3];
    unsigned int span = (unsigned int)(DSC_PAIR_CODE_MAX - DSC_PAIR_CODE_MIN + 1);
    unsigned int n = DSC_PAIR_CODE_MIN + (v % span);
    snprintf(out, DSC_PAIR_CODE_LEN + 1, "%06u", n);
    return DSC_OK;
}

int dsc_pairing_generate(dsc_pairing *p, const char *avoid_code, long long now_ms,
                         char *out_code, size_t cap, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!p) return DSC_E_INVAL;
    for (int attempt = 0; attempt < 64; attempt++) {
        char code[DSC_PAIR_CODE_LEN + 1];
        int rc = make_code(code);
        if (rc != DSC_OK) {
            if (err && errlen) snprintf(err, errlen, "无法获取随机数，配对码生成失败");
            return rc;
        }
        /* 两端码值必须不同：撞上已知对端码就换一个。 */
        if (avoid_code && dsc_pair_code_valid(avoid_code) && strcmp(code, avoid_code) == 0) continue;
        dsc_strlcpy_utf8(p->local_code, sizeof(p->local_code), code);
        p->peer_code[0] = '\0';
        p->fails = 0;
        p->expires_ms = now_ms + DSC_PAIR_CODE_TTL_MS;
        p->state = DSC_PAIR_CODE_READY;
        if (out_code && cap) dsc_strlcpy_utf8(out_code, cap, p->local_code);
        return DSC_OK;
    }
    if (err && errlen) snprintf(err, errlen, "配对码重复生成失败，请重试");
    return DSC_E_STATE;
}

static int read_record(dsc_stmt *st, dsc_pair_record *out)
{
    const char *v;
    memset(out, 0, sizeof(*out));
    if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(out->peer_id, sizeof(out->peer_id), v);
    if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(out->peer_name, sizeof(out->peer_name), v);
    if ((v = dsc_stmt_text(st, 2))) dsc_strlcpy_utf8(out->device_pair_id, sizeof(out->device_pair_id), v);
    if ((v = dsc_stmt_text(st, 3))) dsc_strlcpy_utf8(out->secret_ref, sizeof(out->secret_ref), v);
    if ((v = dsc_stmt_text(st, 4))) dsc_strlcpy_utf8(out->paired_at, sizeof(out->paired_at), v);
    out->last_logical_ts = dsc_stmt_ll(st, 5);
    out->realtime = dsc_stmt_int(st, 6);
    return DSC_OK;
}

#define PAIR_COLS "peer_id, peer_name, device_pair_id, secret_ref, paired_at, last_logical_ts, realtime"

static int establish(dsc_pairing *p, const char *peer_id, const char *peer_name,
                     dsc_pair_record *out, char *err, size_t errlen)
{
    char pair_id[40];
    dsc_uuid4(pair_id);

    unsigned char raw[32];
    if (dsc_random_bytes(raw, sizeof(raw)) != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "无法获取随机数，共享密钥生成失败");
        return DSC_E_IO;
    }
    char secret_hex[65];
    dsc_hex_encode(raw, sizeof(raw), secret_hex);
    dsc_secure_zero(raw, sizeof(raw));

    char cred_err[256];
    cred_err[0] = '\0';
    int rc = dsc_cred_save(DSC_CRED_SERVICE_SYNC, peer_id, secret_hex, cred_err, sizeof(cred_err));
    dsc_secure_zero(secret_hex, sizeof(secret_hex));
    if (rc != DSC_OK) {
        if (err && errlen) {
            snprintf(err, errlen, "共享密钥写入平台安全存储失败：%s", cred_err[0] ? cred_err : dsc_strerror(rc));
        }
        return rc;
    }

    char ref[128];
    snprintf(ref, sizeof(ref), "pair/%s", pair_id);
    char now[32];
    dsc_now_iso8601(now);

    dsc_stmt *st = dsc_db_prepare(p->db,
        "INSERT OR REPLACE INTO device_pair(peer_id, peer_name, device_pair_id, secret_ref, paired_at,"
        " last_logical_ts, realtime) VALUES(?1, ?2, ?3, ?4, ?5, 0, 1);");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, peer_id);
    dsc_stmt_bind_text(st, 2, peer_name ? peer_name : "");
    dsc_stmt_bind_text(st, 3, pair_id);
    dsc_stmt_bind_text(st, 4, ref);
    dsc_stmt_bind_text(st, 5, now);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) return rc;

    dsc_setting_set(p->db, DSC_SET_PAIRING_PEER, peer_id);

    if (out) {
        memset(out, 0, sizeof(*out));
        dsc_strlcpy_utf8(out->peer_id, sizeof(out->peer_id), peer_id);
        dsc_strlcpy_utf8(out->peer_name, sizeof(out->peer_name), peer_name ? peer_name : "");
        dsc_strlcpy_utf8(out->device_pair_id, sizeof(out->device_pair_id), pair_id);
        dsc_strlcpy_utf8(out->secret_ref, sizeof(out->secret_ref), ref);
        dsc_strlcpy_utf8(out->paired_at, sizeof(out->paired_at), now);
        out->realtime = 1;
    }
    return DSC_OK;
}

int dsc_pairing_verify(dsc_pairing *p, const char *peer_id, const char *peer_name,
                       const char *peer_code, const char *peer_entered_our_code,
                       long long now_ms, dsc_pair_record *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!p || !peer_id || !*peer_id || !peer_code || !peer_entered_our_code) return DSC_E_INVAL;
    if (p->state != DSC_PAIR_CODE_READY) {
        if (err && errlen) snprintf(err, errlen, "请先生成本端配对码");
        return DSC_E_STATE;
    }
    if (now_ms >= p->expires_ms) {
        p->state = DSC_PAIR_FAILED;
        dsc_secure_zero(p->local_code, sizeof(p->local_code));
        if (err && errlen) snprintf(err, errlen, "配对码已过期（有效期 5 分钟），请重新生成");
        return DSC_E_TIMEOUT;
    }

    int ok = 1;
    if (!dsc_pair_code_valid(peer_code) || !dsc_pair_code_valid(peer_entered_our_code)) ok = 0;
    else if (strcmp(peer_entered_our_code, p->local_code) != 0) ok = 0;
    if (!ok) {
        p->fails++;
        if (p->fails >= DSC_PAIR_MAX_FAILS) {
            p->state = DSC_PAIR_FAILED;
            dsc_secure_zero(p->local_code, sizeof(p->local_code));
            if (err && errlen) {
                snprintf(err, errlen, "配对码输入错误累计 %d 次，本次配对已作废，请重新生成配对码",
                         DSC_PAIR_MAX_FAILS);
            }
            return DSC_E_AUTH;
        }
        if (err && errlen) {
            snprintf(err, errlen, "配对码不正确，还可尝试 %d 次", DSC_PAIR_MAX_FAILS - p->fails);
        }
        return DSC_E_AUTH;
    }

    dsc_strlcpy_utf8(p->peer_code, sizeof(p->peer_code), peer_code);
    int rc = establish(p, peer_id, peer_name, out, err, errlen);
    if (rc != DSC_OK) return rc;
    p->state = DSC_PAIR_VERIFIED;
    return DSC_OK;
}

int dsc_pairing_current(dsc_db *db, dsc_pair_record *out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT " PAIR_COLS " FROM device_pair ORDER BY paired_at DESC LIMIT 1;");
    if (!st) return DSC_E_DB;
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_record(st, out);
    dsc_stmt_finalize(st);
    if (rc == DSC_ROW) return DSC_OK;
    return rc == DSC_DONE ? DSC_E_NOTFOUND : rc;
}

int dsc_pairing_unpair(dsc_db *db, const char *peer_id, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!db || !peer_id || !*peer_id) return DSC_E_INVAL;

    /* 先删安全存储里的共享密钥，删不掉就不动数据库，避免留下无法解开的配对关系。 */
    char cred_err[256];
    cred_err[0] = '\0';
    int rc = dsc_cred_delete(DSC_CRED_SERVICE_SYNC, peer_id, cred_err, sizeof(cred_err));
    if (rc != DSC_OK && rc != DSC_E_NOTFOUND) {
        if (err && errlen) {
            snprintf(err, errlen, "删除共享密钥失败：%s", cred_err[0] ? cred_err : dsc_strerror(rc));
        }
        return rc;
    }

    dsc_stmt *st = dsc_db_prepare(db, "DELETE FROM device_pair WHERE peer_id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, peer_id);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) return rc;

    dsc_stmt *st2 = dsc_db_prepare(db, "DELETE FROM sync_cursor WHERE peer_id = ?1;");
    if (st2) {
        dsc_stmt_bind_text(st2, 1, peer_id);
        dsc_stmt_step(st2);
        dsc_stmt_finalize(st2);
    }
    dsc_setting_del(db, DSC_SET_PAIRING_PEER);
    if (err && errlen) snprintf(err, errlen, "已解除配对；两端本地数据均保留");
    return DSC_OK;
}
