/* store_profile.c */
#include "store_profile.h"

#include <string.h>

static void trim_and_copy(char *dst, size_t dstlen, const char *src, size_t max_chars)
{
    if (!src) { dst[0] = '\0'; return; }
    char *tmp = dsc_strdup(src);
    size_t n = strlen(tmp);
    while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t' || tmp[n - 1] == '\r' || tmp[n - 1] == '\n')) tmp[--n] = '\0';
    dsc_utf8_truncate_chars(tmp, max_chars);
    dsc_strlcpy_utf8(dst, dstlen, tmp);
    dsc_xfree(tmp);
}

int dsc_profile_validate(const dsc_profile *p, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!p) { if (err && errlen) snprintf(err, errlen, "资料为空"); return DSC_E_INVAL; }

    if (dsc_utf8_length(p->name) > DSC_PROFILE_NAME_MAX) {
        if (err && errlen) snprintf(err, errlen, "昵称最长 %d 个字符", DSC_PROFILE_NAME_MAX);
        return DSC_E_LIMIT;
    }
    if (dsc_utf8_length(p->occupation) > DSC_PROFILE_OCCUPATION_MAX) {
        if (err && errlen) snprintf(err, errlen, "职业最长 %d 个字符", DSC_PROFILE_OCCUPATION_MAX);
        return DSC_E_LIMIT;
    }
    if (dsc_utf8_length(p->extra_note) > DSC_PROFILE_NOTE_MAX) {
        if (err && errlen) snprintf(err, errlen, "补充说明最长 %d 个字符", DSC_PROFILE_NOTE_MAX);
        return DSC_E_LIMIT;
    }
    if (!dsc_utf8_valid(p->name) || !dsc_utf8_valid(p->occupation) || !dsc_utf8_valid(p->extra_note)) {
        if (err && errlen) snprintf(err, errlen, "文本包含非法字符编码");
        return DSC_E_INVAL;
    }
    if (p->gender[0] && strcmp(p->gender, "未设置") != 0 && strcmp(p->gender, "男") != 0 &&
        strcmp(p->gender, "女") != 0 && strcmp(p->gender, "其他") != 0) {
        if (err && errlen) snprintf(err, errlen, "性别取值只能是 未设置/男/女/其他");
        return DSC_E_INVAL;
    }
    if (p->birthday[0]) {
        if (!dsc_is_valid_date(p->birthday)) {
            if (err && errlen) snprintf(err, errlen, "生日格式应为 YYYY-MM-DD 且日期有效");
            return DSC_E_INVAL;
        }
    }
    if (p->avatar_path[0] && !dsc_is_local_relative_path(p->avatar_path)) {
        if (err && errlen) snprintf(err, errlen, "头像必须是应用内的本地相对路径，不允许外链或绝对路径");
        return DSC_E_INVAL;
    }
    return DSC_OK;
}

int dsc_profile_get(dsc_db *db, dsc_profile *out)
{
    if (!db || !out) return DSC_E_INVAL;
    memset(out, 0, sizeof(*out));
    dsc_strlcpy_utf8(out->gender, sizeof(out->gender), "未设置");
    dsc_stmt *st = dsc_db_prepare(db,
        "SELECT name, avatar_path, gender, birthday, occupation, extra_note, updated_at "
        "FROM user_profile WHERE id = 1;");
    if (!st) return DSC_E_DB;
    int rc = dsc_stmt_step(st);
    if (rc == DSC_ROW) {
        const char *v;
        if ((v = dsc_stmt_text(st, 0))) dsc_strlcpy_utf8(out->name, sizeof(out->name), v);
        if ((v = dsc_stmt_text(st, 1))) dsc_strlcpy_utf8(out->avatar_path, sizeof(out->avatar_path), v);
        if ((v = dsc_stmt_text(st, 2)) && *v) dsc_strlcpy_utf8(out->gender, sizeof(out->gender), v);
        if ((v = dsc_stmt_text(st, 3))) dsc_strlcpy_utf8(out->birthday, sizeof(out->birthday), v);
        if ((v = dsc_stmt_text(st, 4))) dsc_strlcpy_utf8(out->occupation, sizeof(out->occupation), v);
        if ((v = dsc_stmt_text(st, 5))) dsc_strlcpy_utf8(out->extra_note, sizeof(out->extra_note), v);
        if ((v = dsc_stmt_text(st, 6))) dsc_strlcpy_utf8(out->updated_at, sizeof(out->updated_at), v);
    }
    dsc_stmt_finalize(st);
    return rc == DSC_ROW || rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_profile_set(dsc_db *db, const dsc_profile *in, char *err, size_t errlen)
{
    if (!db || !in) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    /* 先按原值判断长度上限，避免截断把「超长」悄悄变成「合法」。 */
    if (dsc_utf8_length(in->name) > DSC_PROFILE_NAME_MAX) {
        if (err && errlen) snprintf(err, errlen, "昵称最长 %d 个字符", DSC_PROFILE_NAME_MAX);
        return DSC_E_LIMIT;
    }
    if (dsc_utf8_length(in->occupation) > DSC_PROFILE_OCCUPATION_MAX) {
        if (err && errlen) snprintf(err, errlen, "职业最长 %d 个字符", DSC_PROFILE_OCCUPATION_MAX);
        return DSC_E_LIMIT;
    }
    if (dsc_utf8_length(in->extra_note) > DSC_PROFILE_NOTE_MAX) {
        if (err && errlen) snprintf(err, errlen, "补充说明最长 %d 个字符", DSC_PROFILE_NOTE_MAX);
        return DSC_E_LIMIT;
    }
    dsc_profile clean;
    memset(&clean, 0, sizeof(clean));
    trim_and_copy(clean.name, sizeof(clean.name), in->name, DSC_PROFILE_NAME_MAX);
    trim_and_copy(clean.avatar_path, sizeof(clean.avatar_path), in->avatar_path, DSC_PROFILE_PATH_BYTES);
    trim_and_copy(clean.gender, sizeof(clean.gender), in->gender, 8);
    trim_and_copy(clean.birthday, sizeof(clean.birthday), in->birthday, 16);
    trim_and_copy(clean.occupation, sizeof(clean.occupation), in->occupation, DSC_PROFILE_OCCUPATION_MAX);
    trim_and_copy(clean.extra_note, sizeof(clean.extra_note), in->extra_note, DSC_PROFILE_NOTE_MAX);
    if (!clean.gender[0]) dsc_strlcpy_utf8(clean.gender, sizeof(clean.gender), "未设置");

    int rc = dsc_profile_validate(&clean, err, errlen);
    if (rc != DSC_OK) return rc;

    char now[32];
    dsc_now_iso8601(now);
    dsc_strlcpy_utf8(clean.updated_at, sizeof(clean.updated_at), now);

    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO user_profile (id, name, avatar_path, gender, birthday, occupation, extra_note, updated_at) "
        "VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, avatar_path=excluded.avatar_path, "
        "gender=excluded.gender, birthday=excluded.birthday, occupation=excluded.occupation, "
        "extra_note=excluded.extra_note, updated_at=excluded.updated_at;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, clean.name);
    dsc_stmt_bind_text(st, 2, clean.avatar_path);
    dsc_stmt_bind_text(st, 3, clean.gender);
    dsc_stmt_bind_text(st, 4, clean.birthday);
    dsc_stmt_bind_text(st, 5, clean.occupation);
    dsc_stmt_bind_text(st, 6, clean.extra_note);
    dsc_stmt_bind_text(st, 7, now);
    rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}