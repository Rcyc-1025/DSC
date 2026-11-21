/* plugin.c —— 插件清单、安装、启用/禁用、卸载与提示词片段拼装。 */
#include "plugin.h"
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
    if (!id || !id[0] || strlen(id) >= AIC_PLUGIN_ID_MAX) return 0;
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
        aic_strlcpy_utf8(dst, cap, jv_string(v));
    }
}

int aic_plugin_parse_manifest(const char *json, size_t len, aic_plugin_manifest *out,
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
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件清单不是 JSON 对象");
        return AIC_E_FORMAT;
    }

    const char *v = jv_get_str(root, "id", NULL);
    if (!id_valid(v)) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件清单的 id 非法（只允许字母数字下划线连字符）");
        return AIC_E_FORMAT;
    }
    aic_strlcpy_utf8(out->id, sizeof(out->id), v);

    v = jv_get_str(root, "name", NULL);
    aic_strlcpy_utf8(out->name, sizeof(out->name), v && v[0] ? v : out->id);

    v = jv_get_str(root, "version", NULL);
    if (!aic_version_valid(v)) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件清单的 version 非法");
        return AIC_E_FORMAT;
    }
    aic_strlcpy_utf8(out->version, sizeof(out->version), v);

    v = jv_get_str(root, "author", NULL);
    aic_strlcpy_utf8(out->author, sizeof(out->author), v ? v : "");

    v = jv_get_str(root, "entry", NULL);
    if (!v || !v[0]) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件清单缺少 entry（动态库文件名）");
        return AIC_E_FORMAT;
    }
    if (!aic_is_local_relative_path(v)) {
        jv_free(root);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件 entry 必须是相对路径");
        return AIC_E_FORMAT;
    }
    aic_strlcpy_utf8(out->entry, sizeof(out->entry), v);

    flatten(root, "permissions", out->permissions, sizeof(out->permissions));
    flatten(root, "extension_points", out->extension_points, sizeof(out->extension_points));

    jv_free(root);
    return AIC_OK;
}

/* 递归复制目录（只处理文件与一级子目录，插件目录足够）。 */
static int copy_tree(const char *src_dir, const char *dst_dir)
{
    int rc = aic_mkdir_p(dst_dir);
    if (rc != AIC_OK) return rc;
    sb names;
    sb_init(&names);
    /* aic_dir_list 成功时返回条目数（>=0），只有负值才是错误。 */
    rc = aic_dir_list(src_dir, &names, '\n');
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
            aic_path_join(from, sizeof(from), src_dir, name);
            aic_path_join(to, sizeof(to), dst_dir, name);
            if (aic_is_dir(from)) {
                copy_tree(from, to);
            } else {
                aic_file_copy(from, to);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    sb_free(&names);
    return AIC_OK;
}

static void read_row(aic_stmt *st, aic_plugin *p)
{
    memset(p, 0, sizeof(*p));
    const char *s;
    s = aic_stmt_text(st, 0);  aic_strlcpy_utf8(p->id, sizeof(p->id), s ? s : "");
    s = aic_stmt_text(st, 1);  aic_strlcpy_utf8(p->name, sizeof(p->name), s ? s : "");
    s = aic_stmt_text(st, 2);  aic_strlcpy_utf8(p->version, sizeof(p->version), s ? s : "");
    s = aic_stmt_text(st, 3);  aic_strlcpy_utf8(p->author, sizeof(p->author), s ? s : "");
    s = aic_stmt_text(st, 4);  aic_strlcpy_utf8(p->entry, sizeof(p->entry), s ? s : "");
    s = aic_stmt_text(st, 5);  aic_strlcpy_utf8(p->permissions, sizeof(p->permissions), s ? s : "");
    s = aic_stmt_text(st, 6);  aic_strlcpy_utf8(p->extension_points, sizeof(p->extension_points), s ? s : "");
    s = aic_stmt_text(st, 7);  aic_strlcpy_utf8(p->import_source, sizeof(p->import_source), s ? s : "file");
    p->enabled = aic_stmt_int(st, 8);
    s = aic_stmt_text(st, 9);  aic_strlcpy_utf8(p->installed_at, sizeof(p->installed_at), s ? s : "");
    s = aic_stmt_text(st, 10); aic_strlcpy_utf8(p->dir, sizeof(p->dir), s ? s : "");
}

int aic_plugin_install(aic_db *db, const char *plugins_root, const char *manifest_path,
                       const char *import_source, aic_plugin *out, char *err, size_t errlen)
{
    if (!db || !plugins_root || !manifest_path) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    char *raw = NULL;
    size_t raw_len = 0;
    int rc = aic_file_read_all(manifest_path, &raw, &raw_len);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "读取插件清单失败");
        return rc;
    }
    aic_plugin_manifest mf;
    rc = aic_plugin_parse_manifest(raw, raw_len, &mf, err, errlen);
    aic_xfree(raw);
    if (rc != AIC_OK) return rc;

    /* 已安装则拒绝（升级走卸载再装，避免半途覆盖）。 */
    aic_plugin existing;
    if (aic_plugin_get(db, mf.id, &existing) == AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "插件 %s 已安装（版本 %s）", mf.id, existing.version);
        return AIC_E_CONFLICT;
    }

    /* 源目录：清单所在目录；entry 必须在源目录里存在。 */
    char src_dir[1024];
    aic_strlcpy_utf8(src_dir, sizeof(src_dir), manifest_path);
    char *slash = strrchr(src_dir, '/');
    char *bslash = strrchr(src_dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) *slash = '\0';
    else aic_strlcpy_utf8(src_dir, sizeof(src_dir), ".");

    char entry_path[1024];
    aic_path_join(entry_path, sizeof(entry_path), src_dir, mf.entry);
    if (!aic_path_exists(entry_path)) {
        if (err && errlen) snprintf(err, errlen, "插件入口文件不存在：%s", mf.entry);
        return AIC_E_NOTFOUND;
    }

    char dst_dir[AIC_PLUGIN_PATH_MAX];
    char joined[1024];
    aic_path_join(joined, sizeof(joined), plugins_root, mf.id);
    aic_strlcpy_utf8(dst_dir, sizeof(dst_dir), joined);
    if (aic_path_exists(dst_dir)) {
        /* 上一次残留：先清掉，避免新旧文件混杂。 */
        sb names;
        sb_init(&names);
        aic_dir_list(dst_dir, &names, '\n');
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
                aic_path_join(f, sizeof(f), dst_dir, name);
                aic_file_remove(f);
            }
            if (!nl) break;
            p = nl + 1;
        }
        sb_free(&names);
    }
    rc = copy_tree(src_dir, dst_dir);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "复制插件文件失败");
        return rc;
    }

    char ts[32];
    aic_now_iso8601(ts);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO plugin (" PLUGIN_COLS ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,0,?9,?10);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, mf.id);
    aic_stmt_bind_text(st, 2, mf.name);
    aic_stmt_bind_text(st, 3, mf.version);
    aic_stmt_bind_text(st, 4, mf.author);
    aic_stmt_bind_text(st, 5, mf.entry);
    aic_stmt_bind_text(st, 6, mf.permissions);
    aic_stmt_bind_text(st, 7, mf.extension_points);
    aic_stmt_bind_text(st, 8, import_source && import_source[0] ? import_source : "file");
    aic_stmt_bind_text(st, 9, ts);
    aic_stmt_bind_text(st, 10, dst_dir);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc == AIC_E_NOTFOUND) rc = AIC_OK;
    if (rc != AIC_OK) return rc;

    if (out) {
        memset(out, 0, sizeof(*out));
        aic_strlcpy_utf8(out->id, sizeof(out->id), mf.id);
        aic_strlcpy_utf8(out->name, sizeof(out->name), mf.name);
        aic_strlcpy_utf8(out->version, sizeof(out->version), mf.version);
        aic_strlcpy_utf8(out->author, sizeof(out->author), mf.author);
        aic_strlcpy_utf8(out->entry, sizeof(out->entry), mf.entry);
        aic_strlcpy_utf8(out->permissions, sizeof(out->permissions), mf.permissions);
        aic_strlcpy_utf8(out->extension_points, sizeof(out->extension_points), mf.extension_points);
        aic_strlcpy_utf8(out->import_source, sizeof(out->import_source),
                         import_source && import_source[0] ? import_source : "file");
        aic_strlcpy_utf8(out->dir, sizeof(out->dir), dst_dir);
        aic_strlcpy_utf8(out->installed_at, sizeof(out->installed_at), ts);
        out->enabled = 0;
    }
    return AIC_OK;
}

int aic_plugin_get(aic_db *db, const char *id, aic_plugin *out)
{
    if (!db || !id || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db, "SELECT " PLUGIN_COLS " FROM plugin WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) read_row(st, out);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : (rc == AIC_DONE ? AIC_E_NOTFOUND : rc);
}

int aic_plugin_list(aic_db *db, int enabled, aic_plugin **out, size_t *out_n)
{
    if (!db || !out || !out_n) return AIC_E_INVAL;
    *out = NULL;
    *out_n = 0;
    const char *sql = enabled < 0
        ? "SELECT " PLUGIN_COLS " FROM plugin ORDER BY installed_at ASC;"
        : "SELECT " PLUGIN_COLS " FROM plugin WHERE enabled = ?1 ORDER BY installed_at ASC;";
    aic_stmt *st = aic_db_prepare(db, sql);
    if (!st) return AIC_E_DB;
    if (enabled >= 0) aic_stmt_bind_ll(st, 1, enabled ? 1 : 0);
    size_t cap = 0, n = 0;
    aic_plugin *arr = NULL;
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            arr = (aic_plugin *)aic_xrealloc(arr, cap * sizeof(aic_plugin));
        }
        read_row(st, &arr[n++]);
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE && rc != AIC_ROW) {
        aic_xfree(arr);
        return rc;
    }
    *out = arr;
    *out_n = n;
    return AIC_OK;
}

int aic_plugin_set_enabled(aic_db *db, const char *id, int enabled, char *err, size_t errlen)
{
    if (!db || !id) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    aic_plugin p;
    int rc = aic_plugin_get(db, id, &p);
    if (rc != AIC_OK) return rc;
    if (enabled) {
        char entry[1024];
        aic_path_join(entry, sizeof(entry), p.dir, p.entry);
        if (!aic_path_exists(entry)) {
            if (err && errlen) snprintf(err, errlen, "插件文件缺失：%s", p.entry);
            return AIC_E_NOTFOUND;
        }
    }
    aic_stmt *st = aic_db_prepare(db, "UPDATE plugin SET enabled = ?1 WHERE id = ?2;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_ll(st, 1, enabled ? 1 : 0);
    aic_stmt_bind_text(st, 2, id);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc == AIC_E_NOTFOUND) rc = AIC_OK;
    return rc;
}

int aic_plugin_uninstall(aic_db *db, const char *id, int keep_data, char *err, size_t errlen)
{
    if (!db || !id) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    aic_plugin p;
    int rc = aic_plugin_get(db, id, &p);
    if (rc != AIC_OK) return rc;

    /* 先删库记录（失败就不动文件，避免出现「文件没了记录还在」）。 */
    aic_stmt *st = aic_db_prepare(db, "DELETE FROM plugin WHERE id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_OK && rc != AIC_E_NOTFOUND) return rc;

    if (!keep_data) {
        sb names;
        sb_init(&names);
        aic_dir_list(p.dir, &names, '\n');
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
                aic_path_join(f, sizeof(f), p.dir, name);
                aic_file_remove(f);
            }
            if (!nl) break;
            q = nl + 1;
        }
        sb_free(&names);
    }
    return AIC_OK;
}

int aic_plugin_ext_point_index(const char *name)
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
int aic_plugin_build_fragment(aic_db *db, sb *out, char *err, size_t errlen)
{
    if (!db || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    sb_reset(out);
    aic_plugin *arr = NULL;
    size_t n = 0;
    int rc = aic_plugin_list(db, 1, &arr, &n);
    if (rc != AIC_OK) return rc;
    /* 按扩展点声明的顺序拼接（提示词片段优先）。 */
    for (size_t i = 0; i < n; i++) {
        if (!strstr(arr[i].extension_points, "prompt.fragment")) continue;
        char f[1024];
        aic_path_join(f, sizeof(f), arr[i].dir, "prompt_fragment.txt");
        char *piece = NULL;
        size_t piece_len = 0;
        if (aic_file_read_all(f, &piece, &piece_len) == AIC_OK) {
            sb_appendn(out, piece, piece_len);
            sb_appendc(out, '\n');
        }
        aic_xfree(piece);
    }
    aic_plugin_free_array(arr, n);
    return AIC_OK;
}

void aic_plugin_free(aic_plugin *p)
{
    (void)p;   /* 结构内没有堆内存 */
}

void aic_plugin_free_array(aic_plugin *arr, size_t n)
{
    (void)n;
    aic_xfree(arr);
}

/* 插件进程崩溃或超时后的收尾（FR-05 + 异常处理章）：禁用插件、记录原因，主程序继续。 */
int aic_plugin_disable_after_crash(aic_db *db, const char *id, const char *reason,
                                   char *err, size_t errlen)
{
    if (!db || !id || !id[0]) return AIC_E_INVAL;
    aic_plugin p;
    int rc = aic_plugin_get(db, id, &p);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件不存在");
        return rc;
    }
    rc = aic_plugin_set_enabled(db, id, 0, err, errlen);
    if (rc != AIC_OK) return rc;

    char key[96];
    snprintf(key, sizeof(key), "plugin.crash.%s", id);
    char ts[32];
    aic_now_iso8601(ts);
    char val[512];
    snprintf(val, sizeof(val), "%s %s", ts, (reason && reason[0]) ? reason : "插件异常退出");
    aic_setting_set(db, key, val);
    if (err && errlen) snprintf(err, errlen, "插件「%s」已禁用：%s", p.name, val);
    return AIC_OK;
}
