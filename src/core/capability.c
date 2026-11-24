/* capability.c —— 设备能力授权与注入（§18 / FR-35）。 */
#include "capability.h"

#include <stdio.h>
#include <string.h>

#include "diaglog.h"
#include "error_store.h"
#include "store_setting.h"

typedef struct cap_meta {
    const char *kind;
    const char *name;
    const char *purpose;
    int required;
    int revocable;
} cap_meta;

static const cap_meta g_caps[AIC_CAP_COUNT] = {
    { AIC_CAP_TIME, "时间", "读取本机当前时间，用来排序消息、显示更新时间；不读取任何其它信息。", 1, 0 },
    { AIC_CAP_LOCATION, "位置", "把城市级位置转成一句简短文本随对话发送，用于回答本地相关问题；不记录坐标轨迹。", 0, 1 },
    { AIC_CAP_WEATHER, "天气", "读取本地天气并转成一句简短文本；失败只会提示，不影响聊天。", 0, 1 },
    { AIC_CAP_MEDIA, "设备图片", "只在你手动选中某张图片时读取这一张，并转成文字描述；不会扫描相册。", 0, 1 },
    { AIC_CAP_APPS, "已安装软件", "只在你手动选中条目时读取这几项软件名；不会上传完整清单。", 0, 1 },
    { AIC_CAP_FILES, "本地文件", "只读取你指定的那一个文件的内容摘要，不遍历目录、不递归。", 0, 1 }
};

static aic_cap_provider g_provider;
static int g_has_provider = 0;

size_t aic_cap_count(void) { return AIC_CAP_COUNT; }

const char *aic_cap_kind_at(size_t i)
{
    return i < AIC_CAP_COUNT ? g_caps[i].kind : NULL;
}

const char *aic_cap_kind_valid(const char *kind)
{
    if (!kind || !kind[0]) return NULL;
    for (size_t i = 0; i < AIC_CAP_COUNT; i++) {
        if (strcmp(g_caps[i].kind, kind) == 0) return g_caps[i].kind;
    }
    return NULL;
}

static const cap_meta *meta_of(const char *kind)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!k) return NULL;
    for (size_t i = 0; i < AIC_CAP_COUNT; i++) {
        if (strcmp(g_caps[i].kind, k) == 0) return &g_caps[i];
    }
    return NULL;
}

const char *aic_cap_name(const char *kind)
{
    const cap_meta *m = meta_of(kind);
    return m ? m->name : "";
}

const char *aic_cap_purpose(const char *kind)
{
    const cap_meta *m = meta_of(kind);
    return m ? m->purpose : "";
}

int aic_cap_required(const char *kind)
{
    const cap_meta *m = meta_of(kind);
    return m ? m->required : 0;
}

int aic_cap_revocable(const char *kind)
{
    const cap_meta *m = meta_of(kind);
    return m ? m->revocable : 0;
}

void aic_cap_set_provider(const aic_cap_provider *p)
{
    if (p) { g_provider = *p; g_has_provider = 1; }
    else aic_cap_clear_provider();
}

void aic_cap_clear_provider(void)
{
    memset(&g_provider, 0, sizeof(g_provider));
    g_has_provider = 0;
}

/* ---------- 授权状态 ---------- */

/* 设置页读的 capability.<kind> 键与表保持镜像（§7.2 键名 + §5.2 表）。 */
static void mirror_setting(aic_db *db, const char *kind, const char *state)
{
    if (!db || !kind) return;
    char key[64];
    snprintf(key, sizeof(key), "%s%s", AIC_SET_CAP_PREFIX, kind);
    aic_setting_set(db, key, state);
}

static int ensure_row(aic_db *db, const char *kind)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!db || !k) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO capability_grant (capability, granted, state, requested_at, granted_at,"
        " last_used_at, deny_count) VALUES (?,?,?,?,?,?,0)"
        " ON CONFLICT(capability) DO NOTHING;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, k);
    aic_stmt_bind_ll(st, 2, 0);
    aic_stmt_bind_text(st, 3, "unset");
    aic_stmt_bind_text(st, 4, "");
    aic_stmt_bind_text(st, 5, "");
    aic_stmt_bind_text(st, 6, "");
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : AIC_E_DB;
}

int aic_cap_status_load(aic_db *db, const char *kind, aic_cap_status *out)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!db || !k || !out) return AIC_E_INVAL;
    memset(out, 0, sizeof(*out));
    aic_strlcpy_utf8(out->kind, sizeof(out->kind), k);
    aic_strlcpy_utf8(out->state, sizeof(out->state), "unset");

    aic_stmt *st = aic_db_prepare(db,
        "SELECT granted, state, requested_at, granted_at, last_used_at, deny_count"
        " FROM capability_grant WHERE capability = ?;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, k);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        out->granted = aic_stmt_int(st, 0);
        aic_strlcpy_utf8(out->state, sizeof(out->state), aic_stmt_text(st, 1));
        aic_strlcpy_utf8(out->requested_at, sizeof(out->requested_at), aic_stmt_text(st, 2));
        aic_strlcpy_utf8(out->granted_at, sizeof(out->granted_at), aic_stmt_text(st, 3));
        aic_strlcpy_utf8(out->last_used_at, sizeof(out->last_used_at), aic_stmt_text(st, 4));
        out->deny_count = aic_stmt_int(st, 5);
    } else if (rc != AIC_DONE) {
        aic_stmt_finalize(st);
        return AIC_E_DB;
    }
    aic_stmt_finalize(st);
    /* 时间是必需能力：一旦授予就固定为已授予（§18）。 */
    if (out->granted) aic_strlcpy_utf8(out->state, sizeof(out->state), "granted");
    return AIC_OK;
}

int aic_cap_is_granted(aic_db *db, const char *kind)
{
    aic_cap_status s;
    if (aic_cap_status_load(db, kind, &s) != AIC_OK) return 0;
    return s.granted ? 1 : 0;
}

int aic_cap_request(aic_db *db, const char *kind, char *purpose, size_t purpose_cap)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!db || !k) return AIC_E_INVAL;
    int rc = ensure_row(db, k);
    if (rc != AIC_OK) return rc;
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE capability_grant SET requested_at = ? WHERE capability = ?;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, now);
    aic_stmt_bind_text(st, 2, k);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    if (purpose && purpose_cap) aic_strlcpy_utf8(purpose, purpose_cap, aic_cap_purpose(k));
    aic_log_write("INFO", "capability request kind=%s", k);
    return AIC_OK;
}

int aic_cap_grant(aic_db *db, const char *kind, int64_t now_ms)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!db || !k) return AIC_E_INVAL;
    int rc = ensure_row(db, k);
    if (rc != AIC_OK) return rc;
    char now[32];
    aic_ms_to_iso8601(now_ms > 0 ? now_ms : aic_now_ms(), now);
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE capability_grant SET granted = 1, state = 'granted', granted_at = ?"
        " WHERE capability = ?;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, now);
    aic_stmt_bind_text(st, 2, k);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    mirror_setting(db, k, "granted");
    aic_log_write("INFO", "capability grant kind=%s", k);
    return AIC_OK;
}

int aic_cap_deny(aic_db *db, const char *kind)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!db || !k) return AIC_E_INVAL;
    int rc = ensure_row(db, k);
    if (rc != AIC_OK) return rc;
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE capability_grant SET granted = 0, state = 'denied', deny_count = deny_count + 1"
        " WHERE capability = ?;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, k);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    mirror_setting(db, k, "denied");
    aic_log_write("INFO", "capability deny kind=%s", k);
    return AIC_OK;
}

int aic_cap_revoke(aic_db *db, const char *kind)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!db || !k) return AIC_E_INVAL;
    if (aic_cap_required(k)) return AIC_E_STATE;  /* 时间不可撤销 */
    int rc = ensure_row(db, k);
    if (rc != AIC_OK) return rc;
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE capability_grant SET granted = 0, state = 'unset', granted_at = ''"
        " WHERE capability = ?;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, k);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return AIC_E_DB;
    mirror_setting(db, k, "unset");
    aic_log_write("INFO", "capability revoke kind=%s", k);
    return AIC_OK;
}

int aic_cap_launch_gate(aic_db *db)
{
    return aic_cap_is_granted(db, AIC_CAP_TIME) ? 1 : 0;
}

/* ---------- 取值 ---------- */

static int provider_call(const char *kind, char *buf, size_t cap)
{
    if (!g_has_provider) return AIC_E_STATE;
    const cap_meta *m = meta_of(kind);
    if (!m) return AIC_E_INVAL;
    int (*fn)(char *, size_t, void *) = NULL;
    if (strcmp(m->kind, AIC_CAP_TIME) == 0)          fn = g_provider.time_now;
    else if (strcmp(m->kind, AIC_CAP_LOCATION) == 0) fn = g_provider.location;
    else if (strcmp(m->kind, AIC_CAP_WEATHER) == 0)  fn = g_provider.weather;
    else if (strcmp(m->kind, AIC_CAP_MEDIA) == 0)    fn = g_provider.media_pick;
    else if (strcmp(m->kind, AIC_CAP_APPS) == 0)     fn = g_provider.apps_pick;
    else if (strcmp(m->kind, AIC_CAP_FILES) == 0)    fn = g_provider.files_pick;
    if (!fn) return AIC_E_STATE;
    buf[0] = '\0';
    int rc = fn(buf, cap, g_provider.ud);
    if (rc == AIC_OK && (!buf[0])) return AIC_E_STATE;
    return rc;
}

static void touch_used(aic_db *db, const char *kind)
{
    if (!db) return;
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "UPDATE capability_grant SET last_used_at = ? WHERE capability = ?;");
    if (!st) return;
    aic_stmt_bind_text(st, 1, now);
    aic_stmt_bind_text(st, 2, kind);
    aic_stmt_step(st);
    aic_stmt_finalize(st);
}

int aic_cap_invoke(aic_db *db, const char *kind, const char *data_root, sb *out)
{
    const char *k = aic_cap_kind_valid(kind);
    if (!out) return AIC_E_INVAL;
    sb_reset(out);
    if (!db || !k) return AIC_E_INVAL;
    if (!aic_cap_is_granted(db, k)) return AIC_E_STATE;

    char raw[4096];
    int rc;
    if (strcmp(k, AIC_CAP_TIME) == 0) {
        /* 时间不依赖平台：本地时钟就是真实现。 */
        char iso[32];
        aic_now_iso8601(iso);
        snprintf(raw, sizeof(raw), "%s", iso);
        rc = AIC_OK;
    } else {
        rc = provider_call(k, raw, sizeof(raw));
    }
    if (rc == AIC_OK) {
        sb clean;
        sb_init(&clean);
        aic_error_sanitize(raw, &clean);
        sb_append(out, sb_cstr(&clean));
        sb_free(&clean);
        touch_used(db, k);
        aic_log_write("INFO", "capability use kind=%s status=ok", k);
        return AIC_OK;
    }
    /* 失败：主流程继续，只把能力名与状态写进错误中心（§18 + §9.4）。 */
    aic_log_write("WARN", "capability use kind=%s status=unavailable rc=%d", k, rc);
    if (data_root && data_root[0]) {
        char msg[256];
        snprintf(msg, sizeof(msg), "设备能力「%s」本次没有取到数据", aic_cap_name(k));
        aic_error_record(db, data_root, AIC_ERR_CAPABILITY, "capability.unavailable",
                         msg, NULL, NULL, NULL, NULL);
    }
    return rc;
}

int aic_cap_build_context(aic_db *db, const char *data_root, sb *out)
{
    if (!out) return AIC_E_INVAL;
    sb_reset(out);
    if (!db) return AIC_E_INVAL;

    static const char *labels[AIC_CAP_COUNT] = {
        "时间", "位置", "天气", "设备图片", "已安装软件", "本地文件"
    };
    sb body;
    sb_init(&body);
    int used = 0;
    for (size_t i = 0; i < AIC_CAP_COUNT; i++) {
        const char *k = g_caps[i].kind;
        if (!aic_cap_is_granted(db, k)) continue;
        sb val;
        sb_init(&val);
        int rc = aic_cap_invoke(db, k, data_root, &val);
        if (rc == AIC_OK && val.len > 0) {
            sb_appendf(&body, "%s：%s\n", labels[i], sb_cstr(&val));
            used++;
        }
        sb_free(&val);
    }
    if (used > 0) {
        sb_append(out, "【设备信息（均由用户逐项授权，只保留本次需要的片段）】\n");
        sb_append(out, sb_cstr(&body));
    }
    sb_free(&body);
    return AIC_OK;
}

int aic_cap_read_text_file(aic_db *db, const char *path, sb *out)
{
    if (!out) return AIC_E_INVAL;
    sb_reset(out);
    if (!db || !path || !path[0]) return AIC_E_INVAL;
    if (!aic_cap_is_granted(db, AIC_CAP_FILES)) return AIC_E_STATE;
    if (aic_is_dir(path)) return AIC_E_INVAL;   /* 只读单个文件，不递归 */

    int64_t size = 0;
    int szrc = aic_file_size(path, &size);
    if (szrc != AIC_OK || size < 0) return AIC_E_IO;
    size_t want = (size_t)((size > AIC_CAP_FILE_MAX) ? AIC_CAP_FILE_MAX : size);
    if (want == 0) return AIC_E_STATE;

    char *buf = NULL;
    size_t got = 0;
    int rc = aic_file_read_all(path, &buf, &got);
    if (rc != AIC_OK) { aic_xfree(buf); return rc; }
    size_t n = got > want ? want : got;

    /* 二进制检测：前 512 字节里的 NUL 或高比例控制字符视为二进制，只报长度。 */
    size_t probe = n < 512 ? n : 512;
    int binary = 0;
    for (size_t i = 0; i < probe; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == 0) { binary = 1; break; }
        if (ch < 0x09 || (ch > 0x0d && ch < 0x20)) binary = 1;
    }
    if (binary) {
        sb_appendf(out, "（用户选中的是一个不可读为文本的文件，大小 %zu 字节，未上传内容）", got);
        aic_xfree(buf);
        touch_used(db, AIC_CAP_FILES);
        return AIC_OK;
    }

    sb raw;
    sb_init(&raw);
    sb_appendn(&raw, buf, n);
    aic_xfree(buf);
    sb clean;
    sb_init(&clean);
    aic_error_sanitize(sb_cstr(&raw), &clean);
    sb_free(&raw);
    if (n < got) sb_appendf(out, "（仅取前 %zu 字节，全文 %zu 字节）\n", n, got);
    sb_append(out, sb_cstr(&clean));
    sb_free(&clean);
    touch_used(db, AIC_CAP_FILES);
    aic_log_write("INFO", "capability use kind=%s status=ok", AIC_CAP_FILES);
    return AIC_OK;
}
