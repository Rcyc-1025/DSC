/* plugin.c —— 插件清单、安装、启用/禁用、卸载与提示词片段拼装。 */
#include "plugin.h"
#include "plugin_proc.h"
#include "json.h"
#include "store_setting.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLUGIN_COLS \
    "id, name, version, author, entry_path, permissions, extension_points, import_source, " \
    "enabled, installed_at, data_dir"

static int id_valid(const char *id)
{
    if (!id || !id[0] || strlen(id) >= DSC_PLUGIN_ID_MAX) return 0;
    for (const char *p = id; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                 *p == '_' || *p == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* 把清单里的数组/字符串统一成逗号分隔串。 */
static void flatten(const jv *root, const char *key, char *dst, size_t cap)
{
    dst[0] = '\0';
    const jv *v = jv_get(root, key);
    if (!v) return;
    size_t used = 0;
    if (v->type == JV_ARR) {
        size_t n = jv_len(v);
        for (size_t i = 0; i < n; i++) {
            const jv *it = jv_at(v, i);
            const char *s = NULL;
            if (it && it->type == JV_STR) s = jv_string(it);
            else if (it && it->type == JV_OBJ) {
                /* 扩展点对象：取 point 字段。 */
                s = jv_get_str(it, "point", NULL);
            }
            if (!s || !s[0]) continue;
            size_t len = strlen(s);
            if (used + len + 2 >= cap) break;
            if (used) dst[used++] = ',';
            memcpy(dst + used, s, len);
            used += len;
            dst[used] = '\0';
        }
    } else if (v->type == JV_STR) {
        dsc_strlcpy_utf8(dst, cap, jv_string(v));
    }
}

int dsc_plugin_parse_manifest(const char *json, size_t len, dsc_plugin_manifest *out,
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
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件清单不是 JSON 对象");
        return DSC_E_FORMAT;
    }

    const char *v = jv_get_str(root, "id", NULL);
    if (!id_valid(v)) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件清单的 id 非法（只允许字母数字下划线连字符）");
        return DSC_E_FORMAT;
    }
    dsc_strlcpy_utf8(out->id, sizeof(out->id), v);

    v = jv_get_str(root, "name", NULL);
    dsc_strlcpy_utf8(out->name, sizeof(out->name), v && v[0] ? v : out->id);

    v = jv_get_str(root, "version", NULL);
    if (!dsc_version_valid(v)) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件清单的 version 非法");
        return DSC_E_FORMAT;
    }
    dsc_strlcpy_utf8(out->version, sizeof(out->version), v);

    v = jv_get_str(root, "author", NULL);
    dsc_strlcpy_utf8(out->author, sizeof(out->author), v ? v : "");

    v = jv_get_str(root, "entry", NULL);
    if (!v || !v[0]) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件清单缺少 entry（动态库文件名）");
        return DSC_E_FORMAT;
    }
    if (!dsc_is_local_relative_path(v)) {
        jv_free(root);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件 entry 必须是相对路径");
        return DSC_E_FORMAT;
    }
    dsc_strlcpy_utf8(out->entry, sizeof(out->entry), v);

    flatten(root, "permissions", out->permissions, sizeof(out->permissions));
    flatten(root, "extension_points", out->extension_points, sizeof(out->extension_points));

    v = jv_get_str(root, "sha256", NULL);
    if (v) {
        if (!dsc_hex_valid(v, 64)) {
            jv_free(root);
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件清单的 sha256 必须是 64 位十六进制串");
            return DSC_E_FORMAT;
        }
        dsc_strlcpy_utf8(out->sha256, sizeof(out->sha256), v);
    }

    jv_free(root);
    return DSC_OK;
}

/* 递归复制目录（只处理文件与一级子目录，插件目录足够）。 */
static int copy_tree(const char *src_dir, const char *dst_dir)
{
    int rc = dsc_mkdir_p(dst_dir);
    if (rc != DSC_OK) return rc;
    sb names;
    sb_init(&names);
    /* dsc_dir_list 成功时返回条目数（>=0），只有负值才是错误。 */
    rc = dsc_dir_list(src_dir, &names, '\n');
    if (rc < 0) { sb_free(&names); return rc; }
    const char *p = sb_cstr(&names);
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char name[512];
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, p, len);
        name[len] = '\0';
        if (name[0]) {
            char from[1024], to[1024];
            dsc_path_join(from, sizeof(from), src_dir, name);
            dsc_path_join(to, sizeof(to), dst_dir, name);
            if (dsc_is_dir(from)) {
                copy_tree(from, to);
            } else {
                dsc_file_copy(from, to);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    sb_free(&names);
    return DSC_OK;
}

static void read_row(dsc_stmt *st, dsc_plugin *p)
{
    memset(p, 0, sizeof(*p));
    const char *s;
    s = dsc_stmt_text(st, 0);  dsc_strlcpy_utf8(p->id, sizeof(p->id), s ? s : "");
    s = dsc_stmt_text(st, 1);  dsc_strlcpy_utf8(p->name, sizeof(p->name), s ? s : "");
    s = dsc_stmt_text(st, 2);  dsc_strlcpy_utf8(p->version, sizeof(p->version), s ? s : "");
    s = dsc_stmt_text(st, 3);  dsc_strlcpy_utf8(p->author, sizeof(p->author), s ? s : "");
    s = dsc_stmt_text(st, 4);  dsc_strlcpy_utf8(p->entry, sizeof(p->entry), s ? s : "");
    s = dsc_stmt_text(st, 5);  dsc_strlcpy_utf8(p->permissions, sizeof(p->permissions), s ? s : "");
    s = dsc_stmt_text(st, 6);  dsc_strlcpy_utf8(p->extension_points, sizeof(p->extension_points), s ? s : "");
    s = dsc_stmt_text(st, 7);  dsc_strlcpy_utf8(p->import_source, sizeof(p->import_source), s ? s : "file");
    p->enabled = dsc_stmt_int(st, 8);
    s = dsc_stmt_text(st, 9);  dsc_strlcpy_utf8(p->installed_at, sizeof(p->installed_at), s ? s : "");
    s = dsc_stmt_text(st, 10); dsc_strlcpy_utf8(p->dir, sizeof(p->dir), s ? s : "");
}

int dsc_plugin_install(dsc_db *db, const char *plugins_root, const char *manifest_path,
                       const char *import_source, dsc_plugin *out, char *err, size_t errlen)
{
    return dsc_plugin_install_ex(db, plugins_root, manifest_path, import_source, 0, out, err, errlen);
}

int dsc_plugin_install_ex(dsc_db *db, const char *plugins_root, const char *manifest_path,
                          const char *import_source, int allow_overwrite,
                          dsc_plugin *out, char *err, size_t errlen)
{
    if (!db || !plugins_root || !manifest_path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    char *raw = NULL;
    size_t raw_len = 0;
    int rc = dsc_file_read_all(manifest_path, &raw, &raw_len);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "读取插件清单失败");
        return rc;
    }
    dsc_plugin_manifest mf;
    rc = dsc_plugin_parse_manifest(raw, raw_len, &mf, err, errlen);
    dsc_xfree(raw);
    if (rc != DSC_OK) return rc;

    /* 已安装：默认提示冲突，用户确认覆盖时走更新语义（技术设计 10.2「提示覆盖或取消」）。 */
    dsc_plugin existing;
    int has_existing = (dsc_plugin_get(db, mf.id, &existing) == DSC_OK);
    if (has_existing && !allow_overwrite) {
        if (err && errlen) snprintf(err, errlen, "插件 %s 已安装（版本 %s）", mf.id, existing.version);
        return DSC_E_CONFLICT;
    }

    /* 源目录：清单所在目录；entry 必须在源目录里存在。 */
    char src_dir[1024];
    dsc_strlcpy_utf8(src_dir, sizeof(src_dir), manifest_path);
    char *slash = strrchr(src_dir, '/');
    char *bslash = strrchr(src_dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) *slash = '\0';
    else dsc_strlcpy_utf8(src_dir, sizeof(src_dir), ".");

    char entry_path[1024];
    dsc_path_join(entry_path, sizeof(entry_path), src_dir, mf.entry);
    if (!dsc_path_exists(entry_path)) {
        if (err && errlen) snprintf(err, errlen, "插件入口文件不存在：%s", mf.entry);
        return DSC_E_NOTFOUND;
    }

    /* 清单声明了 sha256 就先校验源文件（技术设计 10.2「校验签名或哈希」）。 */
    if (mf.sha256[0]) {
        char *data = NULL;
        size_t len = 0;
        rc = dsc_file_read_all(entry_path, &data, &len);
        if (rc != DSC_OK) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "读取插件入口文件失败");
            return rc;
        }
        char sha[65];
        dsc_sha256_hex(data, len, sha);
        dsc_xfree(data);
        if (strcmp(sha, mf.sha256) != 0) {
            if (err && errlen) snprintf(err, errlen, "插件入口文件哈希校验失败（期望 %s，实际 %s）", mf.sha256, sha);
            return DSC_E_CHECKSUM;
        }
    }

    char dst_dir[DSC_PLUGIN_PATH_MAX];
    char joined[1024];
    dsc_path_join(joined, sizeof(joined), plugins_root, mf.id);
    dsc_strlcpy_utf8(dst_dir, sizeof(dst_dir), joined);
    if (dsc_path_exists(dst_dir)) {
        /* 上一次残留或覆盖安装：先清掉，避免新旧文件混杂。 */
        sb names;
        sb_init(&names);
        dsc_dir_list(dst_dir, &names, '\n');
        const char *p = sb_cstr(&names);
        while (p && *p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            char name[512];
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, p, len);
            name[len] = '\0';
            if (name[0]) {
                char f[1024];
                dsc_path_join(f, sizeof(f), dst_dir, name);
                dsc_file_remove(f);
            }
            if (!nl) break;
            p = nl + 1;
        }
        sb_free(&names);
    }
    rc = copy_tree(src_dir, dst_dir);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "复制插件文件失败");
        return rc;
    }

    char ts[32];
    dsc_now_iso8601(ts);
    const char *src_label = (import_source && import_source[0]) ? import_source : "file";
    if (has_existing) {
        dsc_stmt *st = dsc_db_prepare(db,
            "UPDATE plugin SET name=?2, version=?3, author=?4, entry_path=?5, permissions=?6, "
            "extension_points=?7, import_source=?8, data_dir=?9 WHERE id=?1;");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, mf.id);
        dsc_stmt_bind_text(st, 2, mf.name);
        dsc_stmt_bind_text(st, 3, mf.version);
        dsc_stmt_bind_text(st, 4, mf.author);
        dsc_stmt_bind_text(st, 5, mf.entry);
        dsc_stmt_bind_text(st, 6, mf.permissions);
        dsc_stmt_bind_text(st, 7, mf.extension_points);
        dsc_stmt_bind_text(st, 8, src_label);
        dsc_stmt_bind_text(st, 9, dst_dir);
        rc = dsc_stmt_step(st);
        dsc_stmt_finalize(st);
        if (rc == DSC_E_NOTFOUND) rc = DSC_OK;
        if (rc != DSC_OK) return rc;
    } else {
        dsc_stmt *st = dsc_db_prepare(db,
            "INSERT INTO plugin (" PLUGIN_COLS ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,0,?9,?10);");
        if (!st) return DSC_E_DB;
        dsc_stmt_bind_text(st, 1, mf.id);
        dsc_stmt_bind_text(st, 2, mf.name);
        dsc_stmt_bind_text(st, 3, mf.version);
        dsc_stmt_bind_text(st, 4, mf.author);
        dsc_stmt_bind_text(st, 5, mf.entry);
        dsc_stmt_bind_text(st, 6, mf.permissions);
        dsc_stmt_bind_text(st, 7, mf.extension_points);
        dsc_stmt_bind_text(st, 8, src_label);
        dsc_stmt_bind_text(st, 9, ts);
        dsc_stmt_bind_text(st, 10, dst_dir);
        rc = dsc_stmt_step(st);
        dsc_stmt_finalize(st);
        if (rc == DSC_E_NOTFOUND) rc = DSC_OK;
        if (rc != DSC_OK) return rc;
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        dsc_strlcpy_utf8(out->id, sizeof(out->id), mf.id);
        dsc_strlcpy_utf8(out->name, sizeof(out->name), mf.name);
        dsc_strlcpy_utf8(out->version, sizeof(out->version), mf.version);
        dsc_strlcpy_utf8(out->author, sizeof(out->author), mf.author);
        dsc_strlcpy_utf8(out->entry, sizeof(out->entry), mf.entry);
        dsc_strlcpy_utf8(out->permissions, sizeof(out->permissions), mf.permissions);
        dsc_strlcpy_utf8(out->extension_points, sizeof(out->extension_points), mf.extension_points);
        dsc_strlcpy_utf8(out->import_source, sizeof(out->import_source), src_label);
        dsc_strlcpy_utf8(out->dir, sizeof(out->dir), dst_dir);
        dsc_strlcpy_utf8(out->installed_at, sizeof(out->installed_at), ts);
        out->enabled = has_existing ? existing.enabled : 0;
    }
    return DSC_OK;
}

int dsc_plugin_get(dsc_db *db, const char *id, dsc_plugin *out)
{
    if (!db || !id || !out) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "SELECT " PLUGIN_COLS " FROM plugin WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) read_row(st, out);
    dsc_stmt_finalize(st);
    return rc == DSC_ROW ? DSC_OK : (rc == DSC_DONE ? DSC_E_NOTFOUND : rc);
}

int dsc_plugin_list(dsc_db *db, int enabled, dsc_plugin **out, size_t *out_n)
{
    if (!db || !out || !out_n) return DSC_E_INVAL;
    *out = NULL;
    *out_n = 0;
    const char *sql = enabled < 0
        ? "SELECT " PLUGIN_COLS " FROM plugin ORDER BY installed_at ASC;"
        : "SELECT " PLUGIN_COLS " FROM plugin WHERE enabled = ?1 ORDER BY installed_at ASC;";
    dsc_stmt *st = dsc_db_prepare(db, sql);
    if (!st) return DSC_E_DB;
    if (enabled >= 0) dsc_stmt_bind_ll(st, 1, enabled ? 1 : 0);
    size_t cap = 0, n = 0;
    dsc_plugin *arr = NULL;
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            arr = (dsc_plugin *)dsc_xrealloc(arr, cap * sizeof(dsc_plugin));
        }
        read_row(st, &arr[n++]);
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE && rc != DSC_ROW) {
        dsc_xfree(arr);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return DSC_OK;
}

int dsc_plugin_set_enabled(dsc_db *db, const char *id, int enabled, char *err, size_t errlen)
{
    if (!db || !id) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    dsc_plugin p;
    int rc = dsc_plugin_get(db, id, &p);
    if (rc != DSC_OK) return rc;
    if (enabled) {
        char entry[1024];
        dsc_path_join(entry, sizeof(entry), p.dir, p.entry);
        if (!dsc_path_exists(entry)) {
            if (err && errlen) snprintf(err, errlen, "插件文件缺失：%s", p.entry);
            return DSC_E_NOTFOUND;
        }
    }
    dsc_stmt *st = dsc_db_prepare(db, "UPDATE plugin SET enabled = ?1 WHERE id = ?2;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_ll(st, 1, enabled ? 1 : 0);
    dsc_stmt_bind_text(st, 2, id);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc == DSC_E_NOTFOUND) rc = DSC_OK;
    return rc;
}

int dsc_plugin_uninstall(dsc_db *db, const char *id, int keep_data, char *err, size_t errlen)
{
    if (!db || !id) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    dsc_plugin p;
    int rc = dsc_plugin_get(db, id, &p);
    if (rc != DSC_OK) return rc;

    /* 先删库记录（失败就不动文件，避免出现「文件没了记录还在」）。 */
    dsc_stmt *st = dsc_db_prepare(db, "DELETE FROM plugin WHERE id = ?1;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, id);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc != DSC_OK && rc != DSC_E_NOTFOUND) return rc;

    if (!keep_data) {
        sb names;
        sb_init(&names);
        dsc_dir_list(p.dir, &names, '\n');
        const char *q = sb_cstr(&names);
        while (q && *q) {
            const char *nl = strchr(q, '\n');
            size_t len = nl ? (size_t)(nl - q) : strlen(q);
            char name[512];
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, q, len);
            name[len] = '\0';
            if (name[0]) {
                char f[1024];
                dsc_path_join(f, sizeof(f), p.dir, name);
                dsc_file_remove(f);
            }
            if (!nl) break;
            q = nl + 1;
        }
        sb_free(&names);
    }
    return DSC_OK;
}

int dsc_plugin_ext_point_index(const char *name)
{
    static const char *points[] = {
        "prompt.fragment",     /* 提示词片段 */
        "ui.panel",            /* 界面扩展面板 */
        "message.transform",   /* 消息渲染前后处理 */
        "tool.provider"        /* 额外工具能力 */
    };
    if (!name) return -1;
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        if (strcmp(points[i], name) == 0) return (int)i;
    }
    return -1;
}

/* 片段文件：插件目录下的 prompt_fragment.txt；缺失时按清单生成一行说明。 */
int dsc_plugin_build_fragment(dsc_db *db, sb *out, char *err, size_t errlen)
{
    if (!db || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    sb_reset(out);
    dsc_plugin *arr = NULL;
    size_t n = 0;
    int rc = dsc_plugin_list(db, 1, &arr, &n);
    if (rc != DSC_OK) return rc;
    /* 按扩展点声明的顺序拼接（提示词片段优先）。 */
    for (size_t i = 0; i < n; i++) {
        if (!strstr(arr[i].extension_points, "prompt.fragment")) continue;
        char f[1024];
        dsc_path_join(f, sizeof(f), arr[i].dir, "prompt_fragment.txt");
        char *piece = NULL;
        size_t piece_len = 0;
        if (dsc_file_read_all(f, &piece, &piece_len) == DSC_OK) {
            sb_appendn(out, piece, piece_len);
            sb_appendc(out, '\n');
        }
        dsc_xfree(piece);
    }
    dsc_plugin_free_array(arr, n);
    return DSC_OK;
}

void dsc_plugin_free(dsc_plugin *p)
{
    (void)p;   /* 结构内没有堆内存 */
}

void dsc_plugin_free_array(dsc_plugin *arr, size_t n)
{
    (void)n;
    dsc_xfree(arr);
}

/* 插件进程崩溃或超时后的收尾（FR-05 + 异常处理章）：禁用插件、记录原因，主程序继续。 */
int dsc_plugin_disable_after_crash(dsc_db *db, const char *id, const char *reason,
                                   char *err, size_t errlen)
{
    if (!db || !id || !id[0]) return DSC_E_INVAL;
    dsc_plugin p;
    int rc = dsc_plugin_get(db, id, &p);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "插件不存在");
        return rc;
    }
    rc = dsc_plugin_set_enabled(db, id, 0, err, errlen);
    if (rc != DSC_OK) return rc;

    char key[96];
    snprintf(key, sizeof(key), "plugin.crash.%s", id);
    char ts[32];
    dsc_now_iso8601(ts);
    char val[512];
    snprintf(val, sizeof(val), "%s %s", ts, (reason && reason[0]) ? reason : "插件异常退出");
    dsc_setting_set(db, key, val);
    if (err && errlen) snprintf(err, errlen, "插件「%s」已禁用：%s", p.name, val);
    return DSC_OK;
}

/* ---------------- 插件更新（技术设计 10.1：安装/启用/禁用/卸载/更新） ---------------- */
int dsc_plugin_update(dsc_db *db, const char *plugins_root, const char *manifest_path,
                      dsc_plugin *out, char *err, size_t errlen)
{
    if (!db || !plugins_root || !manifest_path) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    char *raw = NULL;
    size_t raw_len = 0;
    int rc = dsc_file_read_all(manifest_path, &raw, &raw_len);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "读取插件清单失败");
        return rc;
    }
    dsc_plugin_manifest mf;
    rc = dsc_plugin_parse_manifest(raw, raw_len, &mf, err, errlen);
    dsc_xfree(raw);
    if (rc != DSC_OK) return rc;

    dsc_plugin existing;
    rc = dsc_plugin_get(db, mf.id, &existing);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "插件 %s 尚未安装，请先安装", mf.id);
        return rc;
    }

    char src_dir[1024];
    dsc_strlcpy_utf8(src_dir, sizeof(src_dir), manifest_path);
    char *slash = strrchr(src_dir, '/');
    char *bslash = strrchr(src_dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) *slash = '\0';
    else dsc_strlcpy_utf8(src_dir, sizeof(src_dir), ".");

    char entry_path[1024];
    dsc_path_join(entry_path, sizeof(entry_path), src_dir, mf.entry);
    if (!dsc_path_exists(entry_path)) {
        if (err && errlen) snprintf(err, errlen, "插件入口文件不存在：%s", mf.entry);
        return DSC_E_NOTFOUND;
    }

    char dst_dir[1024];
    char joined[1024];
    dsc_path_join(joined, sizeof(joined), plugins_root, mf.id);
    dsc_strlcpy_utf8(dst_dir, sizeof(dst_dir), joined);
    rc = copy_tree(src_dir, dst_dir);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "复制插件文件失败");
        return rc;
    }

    dsc_stmt *st = dsc_db_prepare(db,
        "UPDATE plugin SET name=?1, version=?2, author=?3, entry_path=?4, permissions=?5, "
        "extension_points=?6 WHERE id=?7;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, mf.name);
    dsc_stmt_bind_text(st, 2, mf.version);
    dsc_stmt_bind_text(st, 3, mf.author);
    dsc_stmt_bind_text(st, 4, mf.entry);
    dsc_stmt_bind_text(st, 5, mf.permissions);
    dsc_stmt_bind_text(st, 6, mf.extension_points);
    dsc_stmt_bind_text(st, 7, mf.id);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    if (rc == DSC_E_NOTFOUND) rc = DSC_OK;
    if (rc != DSC_OK) return rc;
    if (out) dsc_plugin_get(db, mf.id, out);
    return DSC_OK;
}

/* ---------------- 扩展点调用（技术设计 10.2） ---------------- */
static int call_one(dsc_db *db, const dsc_plugin *pl, const char *host_exe, const char *point,
                    const char *op, const char *request_json, jw *w, char *err, size_t errlen)
{
    char entry_abs[1024];
    dsc_path_join(entry_abs, sizeof(entry_abs), pl->dir, pl->entry);
    sb rb;
    sb_init(&rb);
    jw req;
    jw_init(&req, &rb, 0);
    jw_obj_begin(&req, NULL);
    jw_put_str(&req, "point", point);
    jw_put_str(&req, "op", op ? op : "");
    if (request_json && request_json[0]) jw_put_raw(&req, "request", request_json);
    else jw_put_null(&req, "request");
    jw_obj_end(&req);
    const char *body = sb_cstr(&rb);

    dsc_plugin_proc_result res;
    memset(&res, 0, sizeof(res));
    int rc = dsc_plugin_proc_call(host_exe, entry_abs, body, strlen(body), 5000, pl->dir,
                                  &res, err, errlen);
    jw_obj_begin(w, NULL);
    jw_put_str(w, "plugin", pl->id);
    jw_put_bool(w, "ok", rc == DSC_OK);
    if (rc == DSC_OK) jw_put_raw(w, "result", res.text ? res.text : "null");
    else jw_put_str(w, "error", err && err[0] ? err : "插件调用失败");
    jw_obj_end(w);
    if (rc != DSC_OK) {
        /* 插件崩溃/超时：禁用并记录事件，主程序继续（技术设计 10.1）。 */
        dsc_plugin_disable_after_crash(db, pl->id, err && err[0] ? err : "插件调用失败", NULL, 0);
    }
    dsc_plugin_proc_free(&res);
    sb_free(&rb);
    return rc;
}

int dsc_plugin_call_point(dsc_db *db, const char *host_exe, const char *point, const char *op,
                          const char *request_json, char *out, size_t out_cap,
                          char *err, size_t errlen)
{
    if (!db || !host_exe || !point || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (dsc_plugin_ext_point_index(point) < 0) {
        if (err && errlen) snprintf(err, errlen, "未知扩展点：%s", point);
        return DSC_E_INVAL;
    }
    dsc_plugin *arr = NULL;
    size_t n = 0;
    int rc = dsc_plugin_list(db, 1, &arr, &n);
    if (rc != DSC_OK) return rc;
    sb ob;
    sb_init(&ob);
    jw w;
    jw_init(&w, &ob, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "point", point);
    jw_arr_begin(&w, "results");
    int called = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strstr(arr[i].extension_points, point)) continue;
        call_one(db, &arr[i], host_exe, point, op, request_json, &w, err, errlen);
        called++;
    }
    jw_arr_end(&w);
    jw_put_int(&w, "count", called);
    jw_obj_end(&w);
    dsc_strlcpy_utf8(out, out_cap, sb_cstr(&ob));
    sb_free(&ob);
    dsc_plugin_free_array(arr, n);
    return DSC_OK;
}
