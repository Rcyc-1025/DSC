/* backup.c */
#include "backup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "json.h"
#include "util.h"
#include "zip.h"

typedef struct ent {
    char name[512];
    char path[1024];
} ent;

typedef struct ent_list {
    ent   *items;
    size_t n;
    size_t cap;
} ent_list;

static void el_push(ent_list *l, const char *name, const char *path)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (ent *)dsc_xrealloc(l->items, l->cap * sizeof(ent));
    }
    dsc_strlcpy_utf8(l->items[l->n].name, sizeof(l->items[l->n].name), name);
    dsc_strlcpy_utf8(l->items[l->n].path, sizeof(l->items[l->n].path), path);
    l->n++;
}

static void el_free(ent_list *l)
{
    dsc_xfree(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

/* 递归收集目录下的文件，name_prefix 形如 "avatars"、子路径用 /。 */
static void walk(ent_list *l, const char *dir, const char *name_prefix)
{
    if (!dsc_is_dir(dir)) return;
    sb names;
    sb_init(&names);
    int n = dsc_dir_list(dir, &names, '\n');
    if (n <= 0) { sb_free(&names); return; }
    const char *p = sb_cstr(&names);
    size_t len = strlen(p);
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || p[i] == '\n') {
            if (i > start) {
                char name[256];
                size_t k = i - start;
                if (k >= sizeof(name)) k = sizeof(name) - 1;
                memcpy(name, p + start, k);
                name[k] = '\0';
                char full[1024], rel[512];
                dsc_path_join(full, sizeof(full), dir, name);
                snprintf(rel, sizeof(rel), "%s/%s", name_prefix, name);
                if (dsc_is_dir(full)) {
                    char sub[512];
                    snprintf(sub, sizeof(sub), "%s", rel);
                    walk(l, full, sub);
                } else {
                    el_push(l, rel, full);
                }
            }
            start = i + 1;
        }
    }
    sb_free(&names);
}

int dsc_backup_create(const char *root, const char *db_path, const char *app_version,
                      const char *out_zip, dsc_backup_info *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!root || !db_path || !out_zip) return DSC_E_INVAL;

    /* 一致性检查 + WAL 落盘，保证包里的 db 是完整可用的。 */
    dsc_db *db = NULL;
    int rc = dsc_db_open(db_path, &db);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "打开数据库失败，无法备份");
        return rc;
    }
    rc = dsc_db_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    if (rc == DSC_OK) {
        dsc_stmt *st = dsc_db_prepare(db, "PRAGMA quick_check;");
        if (st) {
            if (dsc_stmt_step(st) == DSC_OK) {
                const char *r = dsc_stmt_text(st, 0);
                if (r && !dsc_str_ieq(r, "ok")) {
                    if (err && errlen) snprintf(err, errlen, "数据库一致性检查未通过：%s", r);
                    rc = DSC_E_DB;
                }
            }
            dsc_stmt_finalize(st);
        }
    }
    int db_schema = 0;
    dsc_db_schema_version(db, &db_schema);
    dsc_db_close(db);
    if (rc != DSC_OK) return rc;

    /* §16.6：同步结果不写入备份包。这里复制一份库、清掉与对端同步相关的表，
     * 备份包里只保留本端实体（配对关系、同步游标、事件队列与冲突副本都不进包）。 */
    char tmp_db[1200];
    snprintf(tmp_db, sizeof(tmp_db), "%s.dbtmp", out_zip);
    dsc_file_remove(tmp_db);
    rc = dsc_file_copy(db_path, tmp_db);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "复制数据库用于备份失败");
        return rc;
    }
    dsc_db *tmpd = NULL;
    rc = dsc_db_open(tmp_db, &tmpd);
    if (rc == DSC_OK) {
        rc = dsc_db_exec(tmpd,
            "DELETE FROM sync_event;"
            "DELETE FROM sync_cursor;"
            "DELETE FROM sync_conflict;"
            "DELETE FROM sync_entity_clock;"
            "DELETE FROM device_pair;");
        dsc_db_close(tmpd);
    }
    if (rc != DSC_OK) {
        dsc_file_remove(tmp_db);
        if (err && errlen) snprintf(err, errlen, "清理备份包内的同步数据失败");
        return rc;
    }

    ent_list l;
    memset(&l, 0, sizeof(l));
    el_push(&l, "db/dsc.db", tmp_db);
    char avatars[1024], plugins[1024];
    dsc_path_join(avatars, sizeof(avatars), root, "avatars");
    dsc_path_join(plugins, sizeof(plugins), root, "plugins");
    walk(&l, avatars, "avatars");
    walk(&l, plugins, "plugins");

    /* manifest.json */
    sb mb;
    sb_init(&mb);
    jw w;
    jw_init(&w, &mb, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "format", DSC_BACKUP_FORMAT);
    jw_put_int(&w, "schema_version", DSC_BACKUP_SCHEMA_VER);
    char ts[32];
    dsc_now_iso8601(ts);
    jw_put_str(&w, "created_at", ts);
    jw_put_str(&w, "app_version", app_version ? app_version : "");
    jw_put_int(&w, "db_schema", db_schema);
    jw_put_bool(&w, "has_credential_ref", 1);
    jw_put_str(&w, "credential_note", "凭证保存在系统安全存储，备份包只记录引用，不含密钥");
    jw_arr_begin(&w, "files");
    long long total = 0;
    for (size_t i = 0; i < l.n; i++) {
        char *data = NULL;
        size_t len = 0;
        int r = dsc_file_read_all(l.items[i].path, &data, &len);
        if (r != DSC_OK) {
            if (err && errlen) snprintf(err, errlen, "读取备份文件失败：%s", l.items[i].path);
            sb_free(&mb);
            el_free(&l);
            return r;
        }
        char hex[65];
        dsc_sha256_hex(data, len, hex);
        dsc_xfree(data);
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "name", l.items[i].name);
        jw_put_int(&w, "size", (long long)len);
        jw_put_str(&w, "sha256", hex);
        jw_obj_end(&w);
        total += (long long)len;
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    const char *manifest = sb_cstr(&mb);

    dsc_zip_src *srcs = (dsc_zip_src *)dsc_xcalloc(l.n + 1, sizeof(dsc_zip_src));
    for (size_t i = 0; i < l.n; i++) {
        srcs[i].name = l.items[i].name;
        srcs[i].path = l.items[i].path;
    }
    srcs[l.n].name = "manifest.json";
    srcs[l.n].data = manifest;
    srcs[l.n].len = strlen(manifest);
    rc = dsc_zip_create(srcs, l.n + 1, out_zip, err, errlen);
    dsc_xfree(srcs);
    if (rc == DSC_OK && out) {
        memset(out, 0, sizeof(*out));
        out->schema_version = DSC_BACKUP_SCHEMA_VER;
        dsc_strlcpy_utf8(out->created_at, sizeof(out->created_at), ts);
        dsc_strlcpy_utf8(out->app_version, sizeof(out->app_version), app_version ? app_version : "");
        out->db_schema = db_schema;
        out->file_count = (int)l.n;
        out->total_bytes = total;
    }
    sb_free(&mb);
    el_free(&l);
    dsc_file_remove(tmp_db);
    return rc;
}

/* 读 manifest 并填 info；full_json 可为 NULL。 */
static int read_manifest(const char *zip_path, jv **out_root, dsc_backup_info *out, char *err, size_t errlen)
{
    char *text = NULL;
    size_t len = 0;
    int rc = dsc_zip_read_entry(zip_path, "manifest.json", &text, &len, err, errlen);
    if (rc != DSC_OK) {
        if (err && errlen && !err[0]) dsc_strlcpy_utf8(err, errlen, "备份包缺少 manifest.json");
        return rc == DSC_E_NOTFOUND ? DSC_E_FORMAT : rc;
    }
    jv *root = NULL;
    rc = jv_parse(text, len, &root, err, errlen);
    dsc_xfree(text);
    if (rc != DSC_OK) return rc;
    const char *fmt = jv_get_str(root, "format", "");
    if (strcmp(fmt, DSC_BACKUP_FORMAT) != 0) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "不是 DSC 备份包");
        jv_free(root);
        return DSC_E_FORMAT;
    }
    int ver = (int)jv_get_int(root, "schema_version", -1);
    if (ver != DSC_BACKUP_SCHEMA_VER) {
        if (err && errlen) snprintf(err, errlen, "备份包格式版本不支持：%d（本程序支持 %d）", ver, DSC_BACKUP_SCHEMA_VER);
        jv_free(root);
        return DSC_E_VERSION;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        out->schema_version = ver;
        dsc_strlcpy_utf8(out->created_at, sizeof(out->created_at), jv_get_str(root, "created_at", ""));
        dsc_strlcpy_utf8(out->app_version, sizeof(out->app_version), jv_get_str(root, "app_version", ""));
        out->db_schema = (int)jv_get_int(root, "db_schema", 0);
        const jv *files = jv_get(root, "files");
        if (files) {
            out->file_count = (int)jv_len(files);
            for (size_t i = 0; i < jv_len(files); i++) out->total_bytes += jv_get_ll(jv_at(files, i), "size", 0);
        }
    }
    if (out_root) *out_root = root;
    else jv_free(root);
    return DSC_OK;
}

int dsc_backup_inspect(const char *zip_path, dsc_backup_info *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!zip_path) return DSC_E_INVAL;
    return read_manifest(zip_path, NULL, out, err, errlen);
}

int dsc_backup_verify(const char *zip_path, dsc_backup_info *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    jv *root = NULL;
    int rc = read_manifest(zip_path, &root, out, err, errlen);
    if (rc != DSC_OK) return rc;
    const jv *files = jv_get(root, "files");
    if (!files) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "备份包清单缺少文件列表");
        jv_free(root);
        return DSC_E_FORMAT;
    }
    for (size_t i = 0; i < jv_len(files); i++) {
        const jv *f = jv_at(files, i);
        const char *name = jv_get_str(f, "name", NULL);
        const char *want = jv_get_str(f, "sha256", NULL);
        char *data = NULL;
        size_t len = 0;
        if (!name || !want) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "备份包清单条目缺少名称或哈希");
            jv_free(root);
            return DSC_E_FORMAT;
        }
        rc = dsc_zip_read_entry(zip_path, name, &data, &len, err, errlen);
        if (rc != DSC_OK) { jv_free(root); return rc == DSC_E_NOTFOUND ? DSC_E_FORMAT : rc; }
        char hex[65];
        dsc_sha256_hex(data, len, hex);
        dsc_xfree(data);
        if (!dsc_str_ieq(hex, want)) {
            if (err && errlen) snprintf(err, errlen, "备份包文件校验失败：%s", name);
            jv_free(root);
            return DSC_E_CHECKSUM;
        }
    }
    jv_free(root);
    return DSC_OK;
}

static void copy_into(ent_list *l, const char *src_root, const char *dst_root, const char *sub)
{
    char src[1024], dst[1024];
    dsc_path_join(src, sizeof(src), src_root, sub);
    dsc_path_join(dst, sizeof(dst), dst_root, sub);
    if (!dsc_is_dir(src)) return;
    dsc_mkdir_p(dst);
    sb names;
    sb_init(&names);
    int n = dsc_dir_list(src, &names, '\n');
    if (n > 0) {
        const char *p = sb_cstr(&names);
        size_t len = strlen(p), start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || p[i] == '\n') {
                if (i > start) {
                    char name[256];
                    size_t k = i - start;
                    if (k >= sizeof(name)) k = sizeof(name) - 1;
                    memcpy(name, p + start, k);
                    name[k] = '\0';
                    char s[1024], d[1024], nsub[512];
                    dsc_path_join(s, sizeof(s), src, name);
                    dsc_path_join(d, sizeof(d), dst, name);
                    snprintf(nsub, sizeof(nsub), "%s/%s", sub, name);
                    if (dsc_is_dir(s)) copy_into(l, src_root, dst_root, nsub);
                    else dsc_file_copy(s, d);
                }
                start = i + 1;
            }
        }
    }
    sb_free(&names);
}

int dsc_backup_restore(const char *zip_path, const char *root, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!zip_path || !root) return DSC_E_INVAL;
    int rc = dsc_backup_verify(zip_path, NULL, err, errlen);
    if (rc != DSC_OK) return rc;

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s/backups/restore-%lld", root, (long long)dsc_now_ms());
    rc = dsc_zip_extract(zip_path, tmp, err, errlen);
    if (rc != DSC_OK) return rc;

    char src_db[1024], dst_db[1024];
    dsc_path_join(src_db, sizeof(src_db), tmp, "db/dsc.db");
    dsc_path_join(dst_db, sizeof(dst_db), root, "db/dsc.db");
    if (!dsc_path_exists(src_db)) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "备份包里没有数据库文件");
        return DSC_E_FORMAT;
    }
    char db_dir[1024];
    dsc_path_join(db_dir, sizeof(db_dir), root, "db");
    dsc_mkdir_p(db_dir);
    dsc_file_remove(dst_db);
    char wal[1200];
    snprintf(wal, sizeof(wal), "%s-wal", dst_db);
    dsc_file_remove(wal);
    snprintf(wal, sizeof(wal), "%s-shm", dst_db);
    dsc_file_remove(wal);
    rc = dsc_file_copy(src_db, dst_db);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "恢复数据库失败，原数据保持不变");
        return rc;
    }
    ent_list dummy;
    memset(&dummy, 0, sizeof(dummy));
    copy_into(&dummy, tmp, root, "avatars");
    copy_into(&dummy, tmp, root, "plugins");
    el_free(&dummy);
    return DSC_OK;
}

int dsc_backup_clean(const char *backups_dir, int keep, int *removed, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (removed) *removed = 0;
    if (!backups_dir) return DSC_E_INVAL;
    if (keep < 0) keep = 0;
    sb names;
    sb_init(&names);
    int n = dsc_dir_list(backups_dir, &names, '\n');
    if (n <= 0) { sb_free(&names); return DSC_OK; }
    /* 只处理 .zip，按名称排序（时间戳命名，字典序即时间序）。 */
    char *list[512];
    int cnt = 0;
    const char *p = sb_cstr(&names);
    size_t len = strlen(p), start = 0;
    for (size_t i = 0; i <= len && cnt < 512; i++) {
        if (i == len || p[i] == '\n') {
            if (i > start) {
                char name[256];
                size_t k = i - start;
                if (k >= sizeof(name)) k = sizeof(name) - 1;
                memcpy(name, p + start, k);
                name[k] = '\0';
                size_t nl = strlen(name);
                if (nl > 4 && dsc_str_ieq(name + nl - 4, ".zip")) list[cnt++] = dsc_strdup(name);
            }
            start = i + 1;
        }
    }
    sb_free(&names);
    for (int i = 1; i < cnt; i++) {
        char *k = list[i];
        int j = i - 1;
        while (j >= 0 && strcmp(list[j], k) > 0) { list[j + 1] = list[j]; j--; }
        list[j + 1] = k;
    }
    int to_remove = cnt - keep;
    for (int i = 0; i < to_remove; i++) {
        char path[1024];
        dsc_path_join(path, sizeof(path), backups_dir, list[i]);
        if (dsc_file_remove(path) == DSC_OK && removed) (*removed)++;
    }
    for (int i = 0; i < cnt; i++) dsc_xfree(list[i]);
    return DSC_OK;
}

int dsc_backup_latest(const char *backups_dir, char *out_path, size_t cap,
                      dsc_backup_info *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    sb names;
    sb_init(&names);
    int n = dsc_dir_list(backups_dir, &names, '\n');
    char best[256] = "";
    if (n > 0) {
        const char *p = sb_cstr(&names);
        size_t len = strlen(p), start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || p[i] == '\n') {
                if (i > start) {
                    char name[256];
                    size_t k = i - start;
                    if (k >= sizeof(name)) k = sizeof(name) - 1;
                    memcpy(name, p + start, k);
                    name[k] = '\0';
                    size_t nl = strlen(name);
                    if (nl > 4 && dsc_str_ieq(name + nl - 4, ".zip") && strcmp(name, best) > 0)
                        dsc_strlcpy_utf8(best, sizeof(best), name);
                }
                start = i + 1;
            }
        }
    }
    sb_free(&names);
    if (!best[0]) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "还没有任何备份");
        return DSC_E_NOTFOUND;
    }
    char path[1024];
    dsc_path_join(path, sizeof(path), backups_dir, best);
    if (out_path && cap) dsc_strlcpy_utf8(out_path, cap, path);
    return dsc_backup_inspect(path, out, err, errlen);
}
