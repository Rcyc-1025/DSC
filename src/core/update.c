/* update.c —— 版本检测与更新过程的数据保护。 */
#include "update.h"
#include "json.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define COPY_FIELD(obj, key, dst, errmsg) do { \
    const char *v_ = jv_get_str((obj), (key), NULL); \
    if (!v_ || !v_[0]) { \
        if (err && errlen) aic_strlcpy_utf8(err, errlen, (errmsg)); \
        return AIC_E_FORMAT; \
    } \
    aic_strlcpy_utf8((dst), sizeof(dst), v_); \
} while (0)

int aic_update_parse_manifest(const char *json, size_t len, aic_update_manifest *out,
                              char *err, size_t errlen)
{
    if (!json || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    jv *root = NULL;
    int rc = jv_parse(json, len, &root, err, errlen);
    if (rc != AIC_OK) return rc;
    if (!root || root->type != JV_OBJ) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "更新清单不是 JSON 对象");
        return AIC_E_FORMAT;
    }

    COPY_FIELD(root, "version", out->version, "更新清单缺少 version");
    COPY_FIELD(root, "url", out->url, "更新清单缺少 url");

    const char *v = jv_get_str(root, "channel", NULL);
    aic_strlcpy_utf8(out->channel, sizeof(out->channel), v && v[0] ? v : "stable");
    v = jv_get_str(root, "min_app_version", NULL);
    aic_strlcpy_utf8(out->min_app_version, sizeof(out->min_app_version), v ? v : "0.0.0");
    v = jv_get_str(root, "released_at", NULL);
    aic_strlcpy_utf8(out->released_at, sizeof(out->released_at), v ? v : "");
    v = jv_get_str(root, "notes", NULL);
    aic_strlcpy_utf8(out->notes, sizeof(out->notes), v ? v : "");
    v = jv_get_str(root, "sha256", NULL);
    aic_strlcpy_utf8(out->sha256, sizeof(out->sha256), v ? v : "");
    out->size = jv_get_ll(root, "size", 0);
    out->mandatory = jv_get_bool(root, "mandatory", 0) ? 1 : 0;

    /* 有 sha256 就必须是 64 位十六进制。 */
    if (out->sha256[0] && !aic_hex_valid(out->sha256, 64)) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "更新清单的 sha256 不是 64 位十六进制");
        return AIC_E_FORMAT;
    }
    if (!aic_version_valid(out->version)) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "更新清单的版本号格式非法");
        return AIC_E_FORMAT;
    }
    jv_free(root);
    return AIC_OK;
}

int aic_update_make_plan(const aic_update_manifest *m, const char *current_version,
                         const char *skip_version, aic_update_plan *out)
{
    if (!m || !out) return AIC_E_INVAL;
    memset(out, 0, sizeof(*out));
    out->status = AIC_UPD_NOT_CHECKED;
    aic_strlcpy_utf8(out->current_version, sizeof(out->current_version),
                     current_version ? current_version : "0.0.0");
    aic_strlcpy_utf8(out->target_version, sizeof(out->target_version), m->version);

    int cmp = aic_version_compare(current_version ? current_version : "0.0.0", m->version);
    if (cmp >= 0) {
        out->status = AIC_UPD_UP_TO_DATE;
        return AIC_OK;
    }
    if (skip_version && skip_version[0] && strcmp(skip_version, m->version) == 0 && !m->mandatory) {
        out->status = AIC_UPD_SKIPPED;
        return AIC_OK;
    }
    out->mandatory = m->mandatory;
    out->needs_full_package =
        aic_version_compare(current_version ? current_version : "0.0.0", m->min_app_version) < 0;
    out->status = out->needs_full_package ? AIC_UPD_TOO_OLD : AIC_UPD_AVAILABLE;
    return AIC_OK;
}

int aic_update_verify_file(const aic_update_manifest *m, const char *path,
                           char *err, size_t errlen)
{
    if (!m || !path) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (!aic_path_exists(path)) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "安装包不存在");
        return AIC_E_NOTFOUND;
    }
    if (m->size > 0) {
        int64_t actual = 0;
        if (aic_file_size(path, &actual) != AIC_OK) {
            if (err && errlen) aic_strlcpy_utf8(err, errlen, "读取安装包大小失败");
            return AIC_E_IO;
        }
        if (actual != m->size) {
            if (err && errlen) {
                snprintf(err, errlen, "安装包大小不符（期望 %lld 字节，实际 %lld 字节）",
                         (long long)m->size, (long long)actual);
            }
            return AIC_E_FORMAT;
        }
    }
    if (m->sha256[0]) {
        char *raw = NULL;
        size_t raw_len = 0;
        if (aic_file_read_all(path, &raw, &raw_len) != AIC_OK) {
            if (err && errlen) aic_strlcpy_utf8(err, errlen, "读取安装包失败");
            return AIC_E_IO;
        }
        char hex[65];
        aic_sha256_hex(raw, raw_len, hex);
        aic_xfree(raw);
        if (!aic_str_ieq(hex, m->sha256)) {
            if (err && errlen) aic_strlcpy_utf8(err, errlen, "安装包哈希校验失败");
            return AIC_E_CHECKSUM;
        }
    }
    return AIC_OK;
}

int aic_update_can_apply(int backup_ok, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!backup_ok) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "数据备份未成功，已中止更新，用户数据保持不变");
        return AIC_E_STATE;
    }
    return AIC_OK;
}

int aic_update_download_unavailable(void *ud, const char *url, const char *dst_path,
                                    long long max_size, char *err, size_t errlen)
{
    (void)ud; (void)url; (void)dst_path; (void)max_size;
    if (err && errlen) aic_strlcpy_utf8(err, errlen, "未配置更新下载通道");
    return AIC_E_UNSUPPORTED;
}

int aic_update_download_local_copy(void *ud, const char *url, const char *dst_path,
                                   long long max_size, char *err, size_t errlen)
{
    (void)ud;
    if (!url || !dst_path) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (!aic_path_exists(url)) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "安装包源文件不存在");
        return AIC_E_NOTFOUND;
    }
    int64_t sz = 0;
    if (aic_file_size(url, &sz) != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "读取安装包大小失败");
        return AIC_E_IO;
    }
    if (max_size > 0 && sz > max_size) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "安装包超过允许大小");
        return AIC_E_LIMIT;
    }
    int rc = aic_file_copy(url, dst_path);
    if (rc != AIC_OK && err && errlen) aic_strlcpy_utf8(err, errlen, "复制安装包失败");
    return rc;
}

int aic_update_run(const aic_update_manifest *m, const char *dst_path,
                   aic_update_download_fn dl, void *ud, int backup_ok, int test_mode,
                   char *err, size_t errlen)
{
    if (!m || !dst_path) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    /* FR-08：备份成功是写入前置条件。 */
    int rc = aic_update_can_apply(backup_ok, err, errlen);
    if (rc != AIC_OK) return rc;
    if (!dl) dl = aic_update_download_unavailable;

    rc = dl(ud, m->url, dst_path, m->size, err, errlen);
    if (rc != AIC_OK) return rc;

    rc = aic_update_verify_file(m, dst_path, err, errlen);
    if (rc != AIC_OK) {
        /* 校验失败：中止安装，删除半包，旧版本与用户数据都不动。 */
        aic_file_remove(dst_path);
        return rc;
    }
    if (test_mode) return AIC_OK;   /* 自动验收：只到校验通过为止 */

    /* 真实安装由平台层完成（Windows 替换安装 / Android 系统更新），本层不实现。 */
    if (err && errlen) aic_strlcpy_utf8(err, errlen, "安装动作由平台层执行");
    return AIC_E_UNSUPPORTED;
}

int aic_update_should_check(int64_t last_check_ms, int interval_hours, int64_t now_ms)
{
    if (interval_hours <= 0) return 0;   /* 关闭周期检查 */
    if (last_check_ms <= 0) return 1;
    int64_t interval = (int64_t)interval_hours * 3600LL * 1000LL;
    return (now_ms - last_check_ms) >= interval ? 1 : 0;
}