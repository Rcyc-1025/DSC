/* update.c —— 版本检测与更新过程的数据保护。 */
#include "update.h"
#include "json.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define COPY_FIELD(obj, key, dst, errmsg) do { \
    const char *v_ = jv_get_str((obj), (key), NULL); \
    if (!v_ || !v_[0]) { \
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, (errmsg)); \
        return DSC_E_FORMAT; \
    } \
    dsc_strlcpy_utf8((dst), sizeof(dst), v_); \
} while (0)

int dsc_update_parse_manifest(const char *json, size_t len, dsc_update_manifest *out,
                              char *err, size_t errlen)
{
    if (!json || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    jv *root = NULL;
    int rc = jv_parse(json, len, &root, err, errlen);
    if (rc != DSC_OK) return rc;
    if (!root || root->type != JV_OBJ) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "更新清单不是 JSON 对象");
        return DSC_E_FORMAT;
    }

    COPY_FIELD(root, "version", out->version, "更新清单缺少 version");
    COPY_FIELD(root, "url", out->url, "更新清单缺少 url");

    const char *v = jv_get_str(root, "channel", NULL);
    dsc_strlcpy_utf8(out->channel, sizeof(out->channel), v && v[0] ? v : "stable");
    v = jv_get_str(root, "min_app_version", NULL);
    dsc_strlcpy_utf8(out->min_app_version, sizeof(out->min_app_version), v ? v : "0.0.0");
    v = jv_get_str(root, "released_at", NULL);
    dsc_strlcpy_utf8(out->released_at, sizeof(out->released_at), v ? v : "");
    v = jv_get_str(root, "notes", NULL);
    dsc_strlcpy_utf8(out->notes, sizeof(out->notes), v ? v : "");
    v = jv_get_str(root, "sha256", NULL);
    dsc_strlcpy_utf8(out->sha256, sizeof(out->sha256), v ? v : "");
    out->size = jv_get_ll(root, "size", 0);
    out->mandatory = jv_get_bool(root, "mandatory", 0) ? 1 : 0;

    /* 有 sha256 就必须是 64 位十六进制。 */
    if (out->sha256[0] && !dsc_hex_valid(out->sha256, 64)) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "更新清单的 sha256 不是 64 位十六进制");
        return DSC_E_FORMAT;
    }
    if (!dsc_version_valid(out->version)) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "更新清单的版本号格式非法");
        return DSC_E_FORMAT;
    }
    jv_free(root);
    return DSC_OK;
}

int dsc_update_make_plan(const dsc_update_manifest *m, const char *current_version,
                         const char *skip_version, dsc_update_plan *out)
{
    if (!m || !out) return DSC_E_INVAL;
    memset(out, 0, sizeof(*out));
    out->status = DSC_UPD_NOT_CHECKED;
    dsc_strlcpy_utf8(out->current_version, sizeof(out->current_version),
                     current_version ? current_version : "0.0.0");
    dsc_strlcpy_utf8(out->target_version, sizeof(out->target_version), m->version);

    int cmp = dsc_version_compare(current_version ? current_version : "0.0.0", m->version);
    if (cmp >= 0) {
        out->status = DSC_UPD_UP_TO_DATE;
        return DSC_OK;
    }
    if (skip_version && skip_version[0] && strcmp(skip_version, m->version) == 0 && !m->mandatory) {
        out->status = DSC_UPD_SKIPPED;
        return DSC_OK;
    }
    out->mandatory = m->mandatory;
    out->needs_full_package =
        dsc_version_compare(current_version ? current_version : "0.0.0", m->min_app_version) < 0;
    out->status = out->needs_full_package ? DSC_UPD_TOO_OLD : DSC_UPD_AVAILABLE;
    return DSC_OK;
}

int dsc_update_verify_file(const dsc_update_manifest *m, const char *path,
                           char *err, size_t errlen)
{
    if (!m || !path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (!dsc_path_exists(path)) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "安装包不存在");
        return DSC_E_NOTFOUND;
    }
    if (m->size > 0) {
        int64_t actual = 0;
        if (dsc_file_size(path, &actual) != DSC_OK) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "读取安装包大小失败");
            return DSC_E_IO;
        }
        if (actual != m->size) {
            if (err && errlen) {
                snprintf(err, errlen, "安装包大小不符（期望 %lld 字节，实际 %lld 字节）",
                         (long long)m->size, (long long)actual);
            }
            return DSC_E_FORMAT;
        }
    }
    if (m->sha256[0]) {
        char *raw = NULL;
        size_t raw_len = 0;
        if (dsc_file_read_all(path, &raw, &raw_len) != DSC_OK) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "读取安装包失败");
            return DSC_E_IO;
        }
        char hex[65];
        dsc_sha256_hex(raw, raw_len, hex);
        dsc_xfree(raw);
        if (!dsc_str_ieq(hex, m->sha256)) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "安装包哈希校验失败");
            return DSC_E_CHECKSUM;
        }
    }
    return DSC_OK;
}

int dsc_update_can_apply(int backup_ok, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!backup_ok) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "数据备份未成功，已中止更新，用户数据保持不变");
        return DSC_E_STATE;
    }
    return DSC_OK;
}

int dsc_update_download_unavailable(void *ud, const char *url, const char *dst_path,
                                    long long max_size, char *err, size_t errlen)
{
    (void)ud; (void)url; (void)dst_path; (void)max_size;
    if (err && errlen) dsc_strlcpy_utf8(err, errlen, "未配置更新下载通道");
    return DSC_E_UNSUPPORTED;
}

int dsc_update_download_local_copy(void *ud, const char *url, const char *dst_path,
                                   long long max_size, char *err, size_t errlen)
{
    (void)ud;
    if (!url || !dst_path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (!dsc_path_exists(url)) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "安装包源文件不存在");
        return DSC_E_NOTFOUND;
    }
    int64_t sz = 0;
    if (dsc_file_size(url, &sz) != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "读取安装包大小失败");
        return DSC_E_IO;
    }
    if (max_size > 0 && sz > max_size) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "安装包超过允许大小");
        return DSC_E_LIMIT;
    }
    int rc = dsc_file_copy(url, dst_path);
    if (rc != DSC_OK && err && errlen) dsc_strlcpy_utf8(err, errlen, "复制安装包失败");
    return rc;
}

int dsc_update_run(const dsc_update_manifest *m, const char *dst_path,
                   dsc_update_download_fn dl, void *ud, int backup_ok, int test_mode,
                   char *err, size_t errlen)
{
    if (!m || !dst_path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    /* FR-08：备份成功是写入前置条件。 */
    int rc = dsc_update_can_apply(backup_ok, err, errlen);
    if (rc != DSC_OK) return rc;
    if (!dl) dl = dsc_update_download_unavailable;

    rc = dl(ud, m->url, dst_path, m->size, err, errlen);
    if (rc != DSC_OK) return rc;

    rc = dsc_update_verify_file(m, dst_path, err, errlen);
    if (rc != DSC_OK) {
        /* 校验失败：中止安装，删除半包，旧版本与用户数据都不动。 */
        dsc_file_remove(dst_path);
        return rc;
    }
    if (test_mode) return DSC_OK;   /* 自动验收：只到校验通过为止 */

    /* 真实安装由平台层完成（Windows 替换安装 / Android 系统更新），见 dsc_update_run_install。 */
    if (err && errlen) dsc_strlcpy_utf8(err, errlen, "安装动作由平台层执行");
    return DSC_E_UNSUPPORTED;
}

int dsc_update_run_install(const dsc_update_manifest *m, const char *dst_path,
                           dsc_update_download_fn dl, void *ud, int backup_ok,
                           const char *app_dir, const char *backup_dir, volatile int *cancel,
                           char *err, size_t errlen)
{
    if (!m || !dst_path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    int rc = dsc_update_can_apply(backup_ok, err, errlen);
    if (rc != DSC_OK) return rc;
    if (cancel && *cancel) return DSC_E_TIMEOUT;
    if (!dl) dl = dsc_update_download_unavailable;
    rc = dl(ud, m->url, dst_path, m->size, err, errlen);
    if (rc != DSC_OK) return rc;
    rc = dsc_update_verify_file(m, dst_path, err, errlen);
    if (rc != DSC_OK) {
        dsc_file_remove(dst_path);
        return rc;
    }
#ifdef _WIN32
    if (!app_dir || !backup_dir) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "缺少程序目录或回滚点目录");
        return DSC_E_INVAL;
    }
    return dsc_update_apply_package(dst_path, app_dir, backup_dir, cancel, err, errlen);
#else
    (void)app_dir; (void)backup_dir; (void)cancel;
    if (err && errlen) dsc_strlcpy_utf8(err, errlen, "当前平台由系统安装器完成更新，请在外部执行安装包");
    return DSC_E_UNSUPPORTED;
#endif
}

int dsc_update_should_check(int64_t last_check_ms, int interval_hours, int64_t now_ms)
{
    if (interval_hours <= 0) return 0;   /* 关闭周期检查 */
    if (last_check_ms <= 0) return 1;
    int64_t interval = (int64_t)interval_hours * 3600LL * 1000LL;
    return (now_ms - last_check_ms) >= interval ? 1 : 0;
}