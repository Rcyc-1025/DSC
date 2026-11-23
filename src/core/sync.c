/* sync.c —— 同步引擎实现（技术设计第 16 章，FR-27 / FR-28 / FR-29）。 */
#include "sync.h"
#include "json.h"
#include "pairing.h"
#include "store_setting.h"
#include "sync_clock.h"

#include <stdio.h>
#include <string.h>

struct aic_sync {
    aic_db        *db;
    aic_sync_state state;
};

static const char *g_entity_names[AIC_ENT_COUNT] = {
    "profile", "persona", "prompt_version", "conversation", "message", "setting", "plugin"
};
static const char *g_op_names[AIC_OP_COUNT] = {
    "create", "update", "recall", "edit", "delete"
};

const char *aic_sync_entity_name(aic_sync_entity e)
{
    if ((int)e < 0 || e >= AIC_ENT_COUNT) return "unknown";
    return g_entity_names[e];
}

int aic_sync_entity_parse(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < AIC_ENT_COUNT; i++) {
        if (strcmp(name, g_entity_names[i]) == 0) return i;
    }
    return -1;
}

const char *aic_sync_op_name(aic_sync_op op)
{
    if ((int)op < 0 || op >= AIC_OP_COUNT) return "unknown";
    return g_op_names[op];
}

int aic_sync_op_parse(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < AIC_OP_COUNT; i++) {
        if (strcmp(name, g_op_names[i]) == 0) return i;
    }
    return -1;
}

int aic_sync_create(aic_db *db, aic_sync **out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_sync *s = (aic_sync *)aic_xcalloc(1, sizeof(aic_sync));
    s->db = db;
    s->state = AIC_SYNC_IDLE;
    *out = s;
    return AIC_OK;
}

void aic_sync_destroy(aic_sync *s) { if (s) aic_xfree(s); }

aic_sync_state aic_sync_get_state(const aic_sync *s) { return s ? s->state : AIC_SYNC_IDLE; }

const char *aic_sync_state_name(aic_sync_state st)
{
    switch (st) {
    case AIC_SYNC_IDLE:        return "IDLE";
    case AIC_SYNC_DISCOVERING: return "DISCOVERING";
    case AIC_SYNC_CONNECTING:  return "CONNECTING";
    case AIC_SYNC_PAIRING:     return "PAIRING";
    case AIC_SYNC_PAIRED:      return "PAIRED";
    case AIC_SYNC_SYNCING:     return "SYNCING";
    default:                   return "UNKNOWN";
    }
}

/* §16.3 的合法迁移：IDLE→DISCOVERING；DISCOVERING→CONNECTING|IDLE；
 * CONNECTING→PAIRING|IDLE；PAIRING→PAIRED|IDLE；PAIRED→SYNCING|IDLE；
 * SYNCING→SYNCING（增量）|PAIRED（链路断开）|IDLE（解除配对）。 */
static int transition_ok(aic_sync_state from, aic_sync_state to)
{
    if (from == to) return from == AIC_SYNC_SYNCING || from == AIC_SYNC_PAIRED;
    switch (from) {
    case AIC_SYNC_IDLE:        return to == AIC_SYNC_DISCOVERING;
    /* 被连接方没有主动连接这一步：发现/监听之后直接进入配对（连接方仍是
     * DISCOVERING → CONNECTING → PAIRING）。 */
    case AIC_SYNC_DISCOVERING: return to == AIC_SYNC_CONNECTING || to == AIC_SYNC_PAIRING ||
                                      to == AIC_SYNC_IDLE;
    case AIC_SYNC_CONNECTING:  return to == AIC_SYNC_PAIRING || to == AIC_SYNC_IDLE;
    case AIC_SYNC_PAIRING:     return to == AIC_SYNC_PAIRED || to == AIC_SYNC_IDLE;
    case AIC_SYNC_PAIRED:      return to == AIC_SYNC_SYNCING || to == AIC_SYNC_IDLE;
    case AIC_SYNC_SYNCING:     return to == AIC_SYNC_PAIRED || to == AIC_SYNC_IDLE;
    default:                   return 0;
    }
}

int aic_sync_enter(aic_sync *s, aic_sync_state next, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return AIC_E_INVAL;
    if (!transition_ok(s->state, next)) {
        if (err && errlen) {
            snprintf(err, errlen, "同步状态不能从 %s 直接变为 %s",
                     aic_sync_state_name(s->state), aic_sync_state_name(next));
        }
        return AIC_E_STATE;
    }
    s->state = next;
    return AIC_OK;
}

int aic_sync_realtime_enabled(aic_db *db)
{
    if (!db) return 0;
    return aic_setting_get_int(db, AIC_SET_SYNC_REALTIME, 1) ? 1 : 0;
}

int aic_sync_set_realtime(aic_db *db, int enabled)
{
    if (!db) return AIC_E_INVAL;
    return aic_setting_set_int(db, AIC_SET_SYNC_REALTIME, enabled ? 1 : 0);
}

/* ================= 实体快照 ================= */

/* 参与同步的设置白名单：凭证、配对、设备、账号、更新、崩溃记录一律不跨端。 */
static int setting_syncable(const char *key)
{
    static const char *deny[] = {
        "credential.", "pairing.", "device.", "account.", "update.", "plugin.crash.",
        "sync.realtime_enabled", "first_run_done", NULL
    };
    if (!key || !*key) return 0;
    for (int i = 0; deny[i]; i++) {
        size_t n = strlen(deny[i]);
        if (strncmp(key, deny[i], n) == 0) return 0;
    }
    return 1;
}

static int snapshot_entity(aic_db *db, aic_sync_entity entity, const char *entity_id,
                           sb *out, char *err, size_t errlen);

int aic_sync_capture(aic_sync *s, aic_sync_entity entity, aic_sync_op op,
                     const char *entity_id, char *out_event_id, size_t cap,
                     char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return AIC_E_INVAL;
    sb b;
    sb_init(&b);
    int rc = snapshot_entity(s->db, entity, entity_id, &b, err, errlen);
    if (rc != AIC_OK) {
        sb_free(&b);
        return rc;
    }
    rc = aic_sync_publish(s, entity, op, entity_id, sb_cstr(&b), out_event_id, cap, err, errlen);
    sb_free(&b);
    return rc;
}

int aic_sync_publish(aic_sync *s, aic_sync_entity entity, aic_sync_op op,
                     const char *entity_id, const char *payload,
                     char *out_event_id, size_t cap, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return AIC_E_INVAL;
    if ((int)entity < 0 || entity >= AIC_ENT_COUNT || (int)op < 0 || op >= AIC_OP_COUNT) {
        if (err && errlen) snprintf(err, errlen, "不支持的同步实体或操作");
        return AIC_E_INVAL;
    }
    if (entity == AIC_ENT_SETTING && !setting_syncable(entity_id)) {
        if (err && errlen) snprintf(err, errlen, "设置项 %s 不在同步范围内", entity_id ? entity_id : "");
        return AIC_E_DISABLED;
    }

    long long ts = 0;
    int rc = aic_clock_next(s->db, &ts);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "无法推进逻辑时钟：%s", aic_strerror(rc));
        return rc;
    }
    char dev[AIC_SYNC_DEVICE_MAX];
    rc = aic_device_id(s->db, dev, sizeof(dev));
    if (rc != AIC_OK) return rc;

    char eid[AIC_SYNC_ID_MAX];
    aic_uuid4(eid);

    aic_stmt *st = aic_db_prepare(s->db,
        "INSERT INTO sync_event(event_id, entity, op, entity_id, payload, logical_ts,"
        " origin_device, schema_version, direction, state, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'out', 'pending', ?9);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, eid);
    aic_stmt_bind_text(st, 2, g_entity_names[entity]);
    aic_stmt_bind_text(st, 3, g_op_names[op]);
    aic_stmt_bind_text(st, 4, entity_id ? entity_id : "");
    aic_stmt_bind_text(st, 5, payload ? payload : "{}");
    aic_stmt_bind_ll(st, 6, ts);
    aic_stmt_bind_text(st, 7, dev);
    aic_stmt_bind_ll(st, 8, AIC_SYNC_SCHEMA_VERSION);
    aic_stmt_bind_ll(st, 9, aic_now_ms());
    rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) {
        if (err && errlen) snprintf(err, errlen, "同步事件入队失败：%s", aic_strerror(rc));
        return rc;
    }
    if (out_event_id && cap) aic_strlcpy_utf8(out_event_id, cap, eid);
    return AIC_OK;
}

int aic_sync_outbox_count(aic_db *db, int *out_count)
{
    if (!db || !out_count) return AIC_E_INVAL;
    *out_count = 0;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT COUNT(*) FROM sync_event WHERE direction = 'out' AND state = 'pending';");
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) *out_count = aic_stmt_int(st, 0);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : AIC_E_DB;
}

void aic_sync_event_free(aic_sync_event *ev)
{
    if (!ev) return;
    aic_xfree(ev->payload);
    memset(ev, 0, sizeof(*ev));
}

int aic_sync_outbox_next(aic_sync *s, aic_sync_event *out)
{
    if (!s || !out) return AIC_E_INVAL;
    memset(out, 0, sizeof(*out));
    aic_stmt *st = aic_db_prepare(s->db,
        "SELECT event_id, entity, op, entity_id, payload, logical_ts, origin_device, schema_version"
        " FROM sync_event WHERE direction = 'out' AND state = 'pending'"
        " ORDER BY logical_ts ASC, created_at ASC LIMIT 1;");
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    if (rc != AIC_ROW) {
        aic_stmt_finalize(st);
        return rc == AIC_DONE ? AIC_E_NOTFOUND : rc;
    }
    const char *v;
    if ((v = aic_stmt_text(st, 0))) aic_strlcpy_utf8(out->event_id, sizeof(out->event_id), v);
    if ((v = aic_stmt_text(st, 1))) aic_strlcpy_utf8(out->entity, sizeof(out->entity), v);
    if ((v = aic_stmt_text(st, 2))) aic_strlcpy_utf8(out->op, sizeof(out->op), v);
    if ((v = aic_stmt_text(st, 3))) aic_strlcpy_utf8(out->entity_id, sizeof(out->entity_id), v);
    if ((v = aic_stmt_text(st, 4))) out->payload = aic_strdup(v);
    out->logical_ts = aic_stmt_ll(st, 5);
    if ((v = aic_stmt_text(st, 6))) aic_strlcpy_utf8(out->origin_device, sizeof(out->origin_device), v);
    out->schema_version = aic_stmt_int(st, 7);
    aic_stmt_finalize(st);
    return AIC_OK;
}

int aic_sync_outbox_mark_sent(aic_sync *s, const char *event_id)
{
    if (!s || !event_id) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(s->db,
        "UPDATE sync_event SET state = 'sent' WHERE event_id = ?1 AND direction = 'out';");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, event_id);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return rc;
    return aic_db_changes(s->db) > 0 ? AIC_OK : AIC_E_NOTFOUND;
}

/* ================= 序列化 ================= */

int aic_sync_event_to_json(const aic_sync_event *ev, sb *out)
{
    if (!ev || !out) return AIC_E_INVAL;
    jw w;
    sb tmp;
    sb_init(&tmp);
    jw_init(&w, &tmp, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "event_id", ev->event_id);
    jw_put_str(&w, "entity", ev->entity);
    jw_put_str(&w, "op", ev->op);
    jw_put_str(&w, "entity_id", ev->entity_id);
    jw_put_raw(&w, "payload", ev->payload && *ev->payload ? ev->payload : "{}");
    jw_put_int(&w, "logical_ts", ev->logical_ts);
    jw_put_str(&w, "origin_device", ev->origin_device);
    jw_put_int(&w, "schema_version", ev->schema_version ? ev->schema_version : AIC_SYNC_SCHEMA_VERSION);
    jw_obj_end(&w);
    sb_append(out, sb_cstr(&tmp));
    sb_free(&tmp);
    return AIC_OK;
}

int aic_sync_event_from_json(const char *text, size_t len, aic_sync_event *ev,
                             char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!text || !ev) return AIC_E_INVAL;
    memset(ev, 0, sizeof(*ev));
    jv *root = NULL;
    char jerr[256];
    jerr[0] = '\0';
    int rc = jv_parse(text, len, &root, jerr, sizeof(jerr));
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "同步事件不是合法 JSON：%s", jerr);
        return AIC_E_FORMAT;
    }
    const char *v;
    if ((v = jv_get_str(root, "event_id", NULL))) aic_strlcpy_utf8(ev->event_id, sizeof(ev->event_id), v);
    if ((v = jv_get_str(root, "entity", NULL))) aic_strlcpy_utf8(ev->entity, sizeof(ev->entity), v);
    if ((v = jv_get_str(root, "op", NULL))) aic_strlcpy_utf8(ev->op, sizeof(ev->op), v);
    if ((v = jv_get_str(root, "entity_id", NULL))) aic_strlcpy_utf8(ev->entity_id, sizeof(ev->entity_id), v);
    if ((v = jv_get_str(root, "origin_device", NULL))) aic_strlcpy_utf8(ev->origin_device, sizeof(ev->origin_device), v);
    ev->logical_ts = jv_get_ll(root, "logical_ts", 0);
    ev->schema_version = (int)jv_get_ll(root, "schema_version", AIC_SYNC_SCHEMA_VERSION);

    const jv *pl = jv_get(root, "payload");
    sb b;
    sb_init(&b);
    if (pl) jv_serialize(pl, &b);
    if (sb_cstr(&b)[0] == '\0') sb_append(&b, "{}");
    ev->payload = aic_strdup(sb_cstr(&b));
    sb_free(&b);

    int ok = 1;
    if (!aic_uuid_valid(ev->event_id)) ok = 0;
    if (aic_sync_entity_parse(ev->entity) < 0) ok = 0;
    if (aic_sync_op_parse(ev->op) < 0) ok = 0;
    if (!ok) {
        aic_sync_event_free(ev);
        if (err && errlen) snprintf(err, errlen, "同步事件缺少必要字段或取值非法");
        jv_free(root);
        return AIC_E_FORMAT;
    }
    jv_free(root);
    return AIC_OK;
}

/* ================= 实体快照（读库 → JSON） ================= */

static void put_or_null(jw *w, const char *key, const char *v)
{
    if (v) jw_put_str(w, key, v);
    else jw_put_null(w, key);
}

static int col_str(aic_stmt *st, int i, char *out, size_t cap)
{
    const char *v = aic_stmt_text(st, i);
    aic_strlcpy_utf8(out, cap, v ? v : "");
    return 0;
}

static int snapshot_entity(aic_db *db, aic_sync_entity entity, const char *entity_id,
                           sb *out, char *err, size_t errlen)
{
    char sql[512];
    char idbuf[AIC_SYNC_ENTITY_ID_MAX];
    aic_strlcpy_utf8(idbuf, sizeof(idbuf), entity_id ? entity_id : "");

    switch (entity) {
    case AIC_ENT_PROFILE:
        snprintf(sql, sizeof(sql),
            "SELECT name, avatar_path, gender, birthday, occupation, extra_note, updated_at"
            " FROM user_profile WHERE id = 1;");
        break;
    case AIC_ENT_PERSONA:
        snprintf(sql, sizeof(sql),
            "SELECT id, name, description, base_prompt, current_version, created_at, updated_at"
            " FROM persona WHERE id = ?1;");
        break;
    case AIC_ENT_PROMPT_VERSION:
        snprintf(sql, sizeof(sql),
            "SELECT id, persona_id, version, content, created_at FROM prompt_version WHERE id = ?1;");
        break;
    case AIC_ENT_CONVERSATION:
        snprintf(sql, sizeof(sql),
            "SELECT id, mode, title, persona_id, model_id, pinned, last_seq, created_at, updated_at"
            " FROM conversation WHERE id = ?1;");
        break;
    case AIC_ENT_MESSAGE:
        snprintf(sql, sizeof(sql),
            "SELECT id, conversation_id, seq, role, content, recalled, edited_at, original_content,"
            " model_id, created_at, superseded, logical_ts, origin_device FROM message WHERE id = ?1;");
        break;
    case AIC_ENT_SETTING:
        if (!setting_syncable(idbuf)) {
            if (err && errlen) snprintf(err, errlen, "设置项 %s 不在同步范围内", idbuf);
            return AIC_E_DISABLED;
        }
        snprintf(sql, sizeof(sql), "SELECT key, value FROM setting WHERE key = ?1;");
        break;
    case AIC_ENT_PLUGIN:
        snprintf(sql, sizeof(sql),
            "SELECT id, name, version, author, entry_path, permissions, extension_points,"
            " import_source, enabled, installed_at, data_dir FROM plugin WHERE id = ?1;");
        break;
    default:
        if (err && errlen) snprintf(err, errlen, "不支持的同步实体");
        return AIC_E_INVAL;
    }

    aic_stmt *st = aic_db_prepare(db, sql);
    if (!st) return AIC_E_DB;
    if (entity != AIC_ENT_PROFILE) aic_stmt_bind_text(st, 1, idbuf);
    int rc = aic_stmt_step(st);
    if (rc != AIC_ROW) {
        aic_stmt_finalize(st);
        if (err && errlen) {
            snprintf(err, errlen, "要同步的 %s 记录不存在：%s", g_entity_names[entity], idbuf);
        }
        return rc == AIC_DONE ? AIC_E_NOTFOUND : rc;
    }

    jw w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);

    switch (entity) {
    case AIC_ENT_PROFILE: {
        char name[128], avatar[512], gender[32], birthday[32], occ[256], note[512], updated[32];
        col_str(st, 0, name, sizeof(name));
        col_str(st, 1, avatar, sizeof(avatar));
        col_str(st, 2, gender, sizeof(gender));
        col_str(st, 3, birthday, sizeof(birthday));
        col_str(st, 4, occ, sizeof(occ));
        col_str(st, 5, note, sizeof(note));
        col_str(st, 6, updated, sizeof(updated));
        jw_put_str(&w, "name", name);
        jw_put_str(&w, "avatar_path", avatar);
        jw_put_str(&w, "gender", gender);
        jw_put_str(&w, "birthday", birthday);
        jw_put_str(&w, "occupation", occ);
        jw_put_str(&w, "extra_note", note);
        jw_put_str(&w, "updated_at", updated);
        break;
    }
    case AIC_ENT_PERSONA: {
        char id[64], name[128], desc[512], base[4096], created[32], updated[32];
        col_str(st, 0, id, sizeof(id));
        col_str(st, 1, name, sizeof(name));
        col_str(st, 2, desc, sizeof(desc));
        col_str(st, 3, base, sizeof(base));
        col_str(st, 4, created, sizeof(created));
        col_str(st, 5, updated, sizeof(updated));
        jw_put_str(&w, "id", id);
        jw_put_str(&w, "name", name);
        jw_put_str(&w, "description", desc);
        jw_put_str(&w, "base_prompt", base);
        jw_put_int(&w, "current_version", aic_stmt_ll(st, 4));
        jw_put_str(&w, "created_at", created);
        jw_put_str(&w, "updated_at", updated);
        break;
    }
    case AIC_ENT_PROMPT_VERSION: {
        char id[64], pid[64], content[4096], created[32];
        col_str(st, 0, id, sizeof(id));
        col_str(st, 1, pid, sizeof(pid));
        col_str(st, 3, content, sizeof(content));
        col_str(st, 4, created, sizeof(created));
        jw_put_str(&w, "id", id);
        jw_put_str(&w, "persona_id", pid);
        jw_put_int(&w, "version", aic_stmt_ll(st, 2));
        jw_put_str(&w, "content", content);
        jw_put_str(&w, "created_at", created);
        break;
    }
    case AIC_ENT_CONVERSATION: {
        char id[64], mode[8], title[512], pid[64], model[64], created[32], updated[32];
        col_str(st, 0, id, sizeof(id));
        col_str(st, 1, mode, sizeof(mode));
        col_str(st, 2, title, sizeof(title));
        col_str(st, 3, pid, sizeof(pid));
        col_str(st, 4, model, sizeof(model));
        col_str(st, 7, created, sizeof(created));
        col_str(st, 8, updated, sizeof(updated));
        jw_put_str(&w, "id", id);
        jw_put_str(&w, "mode", mode);
        jw_put_str(&w, "title", title);
        jw_put_str(&w, "persona_id", pid);
        jw_put_str(&w, "model_id", model);
        jw_put_int(&w, "pinned", aic_stmt_ll(st, 5));
        jw_put_int(&w, "last_seq", aic_stmt_ll(st, 6));
        jw_put_str(&w, "created_at", created);
        jw_put_str(&w, "updated_at", updated);
        break;
    }
    case AIC_ENT_MESSAGE: {
        char id[64], cid[64], role[16], content[65536], created[32], dev[40];
        char orig[65536];
        col_str(st, 0, id, sizeof(id));
        col_str(st, 1, cid, sizeof(cid));
        col_str(st, 3, role, sizeof(role));
        col_str(st, 4, content, sizeof(content));
        col_str(st, 9, created, sizeof(created));
        col_str(st, 12, dev, sizeof(dev));
        orig[0] = '\0';
        if (!aic_stmt_is_null(st, 7)) col_str(st, 7, orig, sizeof(orig));
        jw_put_str(&w, "id", id);
        jw_put_str(&w, "conversation_id", cid);
        jw_put_int(&w, "seq", aic_stmt_ll(st, 2));
        jw_put_str(&w, "role", role);
        jw_put_str(&w, "content", content);
        jw_put_int(&w, "recalled", aic_stmt_ll(st, 5));
        put_or_null(&w, "edited_at", aic_stmt_is_null(st, 6) ? NULL : aic_stmt_text(st, 6));
        put_or_null(&w, "original_content", orig[0] ? orig : NULL);
        put_or_null(&w, "model_id", aic_stmt_is_null(st, 8) ? NULL : aic_stmt_text(st, 8));
        jw_put_str(&w, "created_at", created);
        jw_put_int(&w, "superseded", aic_stmt_ll(st, 10));
        jw_put_int(&w, "logical_ts", aic_stmt_ll(st, 11));
        jw_put_str(&w, "origin_device", dev);
        break;
    }
    case AIC_ENT_SETTING: {
        char key[256], value[4096];
        col_str(st, 0, key, sizeof(key));
        col_str(st, 1, value, sizeof(value));
        jw_put_str(&w, "key", key);
        jw_put_str(&w, "value", value);
        break;
    }
    case AIC_ENT_PLUGIN: {
        char id[64], name[128], ver[32], author[128], entry[512], perms[512], exts[512];
        char src[32], installed[32], data_dir[512];
        col_str(st, 0, id, sizeof(id));
        col_str(st, 1, name, sizeof(name));
        col_str(st, 2, ver, sizeof(ver));
        col_str(st, 3, author, sizeof(author));
        col_str(st, 4, entry, sizeof(entry));
        col_str(st, 5, perms, sizeof(perms));
        col_str(st, 6, exts, sizeof(exts));
        col_str(st, 7, src, sizeof(src));
        col_str(st, 9, installed, sizeof(installed));
        col_str(st, 10, data_dir, sizeof(data_dir));
        jw_put_str(&w, "id", id);
        jw_put_str(&w, "name", name);
        jw_put_str(&w, "version", ver);
        jw_put_str(&w, "author", author);
        jw_put_str(&w, "entry_path", entry);
        jw_put_str(&w, "permissions", perms);
        jw_put_str(&w, "extension_points", exts);
        jw_put_str(&w, "import_source", src);
        jw_put_int(&w, "enabled", aic_stmt_ll(st, 8));
        jw_put_str(&w, "installed_at", installed);
        jw_put_str(&w, "data_dir", data_dir);
        break;
    }
    default:
        break;
    }
    jw_obj_end(&w);
    aic_stmt_finalize(st);
    return AIC_OK;
}

/* ================= 实体逻辑时钟 ================= */

static int entity_clock_get(aic_db *db, const char *entity, const char *entity_id,
                            long long *out_ts, char *out_dev, size_t cap)
{
    *out_ts = 0;
    if (out_dev && cap) out_dev[0] = '\0';
    aic_stmt *st = aic_db_prepare(db,
        "SELECT logical_ts, origin_device FROM sync_entity_clock WHERE entity = ?1 AND entity_id = ?2;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, entity);
    aic_stmt_bind_text(st, 2, entity_id ? entity_id : "");
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        *out_ts = aic_stmt_ll(st, 0);
        if (out_dev && cap) {
            const char *v = aic_stmt_text(st, 1);
            aic_strlcpy_utf8(out_dev, cap, v ? v : "");
        }
        rc = AIC_OK;
    } else {
        rc = AIC_OK;
    }
    aic_stmt_finalize(st);
    return rc;
}

static int entity_clock_set(aic_db *db, const char *entity, const char *entity_id,
                            long long ts, const char *dev)
{
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO sync_entity_clock(entity, entity_id, logical_ts, origin_device)"
        " VALUES(?1, ?2, ?3, ?4)"
        " ON CONFLICT(entity, entity_id) DO UPDATE SET logical_ts = excluded.logical_ts,"
        " origin_device = excluded.origin_device;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, entity);
    aic_stmt_bind_text(st, 2, entity_id ? entity_id : "");
    aic_stmt_bind_ll(st, 3, ts);
    aic_stmt_bind_text(st, 4, dev ? dev : "");
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

/* ================= 冲突副本 ================= */

static int save_conflict(aic_db *db, const aic_sync_event *ev)
{
    char id[40];
    aic_uuid4(id);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO sync_conflict(id, event_id, entity, entity_id, payload, logical_ts,"
        " origin_device, created_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, ev->event_id);
    aic_stmt_bind_text(st, 3, ev->entity);
    aic_stmt_bind_text(st, 4, ev->entity_id);
    aic_stmt_bind_text(st, 5, ev->payload ? ev->payload : "{}");
    aic_stmt_bind_ll(st, 6, ev->logical_ts);
    aic_stmt_bind_text(st, 7, ev->origin_device);
    aic_stmt_bind_ll(st, 8, aic_now_ms());
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_sync_conflict_count(aic_db *db, int *out_count)
{
    if (!db || !out_count) return AIC_E_INVAL;
    *out_count = 0;
    aic_stmt *st = aic_db_prepare(db, "SELECT COUNT(*) FROM sync_conflict;");
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) *out_count = aic_stmt_int(st, 0);
    aic_stmt_finalize(st);
    return rc == AIC_ROW ? AIC_OK : AIC_E_DB;
}

int aic_sync_conflicts_json(aic_db *db, sb *out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT entity, entity_id, logical_ts, origin_device, created_at FROM sync_conflict"
        " ORDER BY created_at DESC LIMIT 100;");
    if (!st) return AIC_E_DB;
    jw w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w, NULL);
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        const char *ent = aic_stmt_text(st, 0);
        const char *eid = aic_stmt_text(st, 1);
        const char *dev = aic_stmt_text(st, 3);
        const char *created = aic_stmt_text(st, 4);
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "entity", ent ? ent : "");
        jw_put_str(&w, "entity_id", eid ? eid : "");
        jw_put_int(&w, "logical_ts", aic_stmt_ll(st, 2));
        jw_put_str(&w, "origin_device", dev ? dev : "");
        jw_put_str(&w, "created_at", created ? created : "");
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_sync_cursor_get(aic_db *db, const char *peer_id, char *out_event, size_t cap,
                        long long *out_ts)
{
    if (!db || !peer_id) return AIC_E_INVAL;
    if (out_event && cap) out_event[0] = '\0';
    if (out_ts) *out_ts = 0;
    aic_stmt *st = aic_db_prepare(db,
        "SELECT applied_event_id, applied_ts FROM sync_cursor WHERE peer_id = ?1;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, peer_id);
    int rc = aic_stmt_step(st);
    if (rc == AIC_ROW) {
        const char *v = aic_stmt_text(st, 0);
        if (out_event && cap) aic_strlcpy_utf8(out_event, cap, v ? v : "");
        if (out_ts) *out_ts = aic_stmt_ll(st, 1);
        rc = AIC_OK;
    } else if (rc == AIC_DONE) {
        rc = AIC_E_NOTFOUND;
    }
    aic_stmt_finalize(st);
    return rc;
}

static int cursor_advance(aic_db *db, const char *peer_id, const char *event_id, long long ts)
{
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO sync_cursor(peer_id, applied_event_id, applied_ts, last_logical_ts)"
        " VALUES(?1, ?2, ?3, ?3)"
        " ON CONFLICT(peer_id) DO UPDATE SET applied_event_id = excluded.applied_event_id,"
        " applied_ts = excluded.applied_ts,"
        " last_logical_ts = MAX(sync_cursor.last_logical_ts, excluded.last_logical_ts);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, peer_id);
    aic_stmt_bind_text(st, 2, event_id ? event_id : "");
    aic_stmt_bind_ll(st, 3, ts);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

/* ================= 应用 payload ================= */

/* chat 会话单例：远端 chat 会话并入本地，保证双端都是"唯一活动会话"。 */
static int local_chat_id(aic_db *db, const char *except_id, char *out, size_t cap)
{
    aic_stmt *st = aic_db_prepare(db, "SELECT id FROM conversation WHERE mode = 'chat' LIMIT 1;");
    if (!st) return AIC_E_DB;
    int rc = aic_stmt_step(st);
    int found = 0;
    if (rc == AIC_ROW) {
        const char *v = aic_stmt_text(st, 0);
        if (v && (!except_id || strcmp(v, except_id) != 0)) {
            aic_strlcpy_utf8(out, cap, v);
            found = 1;
        }
    }
    aic_stmt_finalize(st);
    return found;
}

static int apply_message(aic_db *db, const aic_sync_event *ev, const jv *p, char *err, size_t errlen)
{
    const char *id = jv_get_str(p, "id", ev->entity_id);
    if (!id || !*id) {
        if (err && errlen) snprintf(err, errlen, "消息事件缺少 id");
        return AIC_E_FORMAT;
    }
    if (aic_sync_op_parse(ev->op) == AIC_OP_DELETE) {
        aic_stmt *st = aic_db_prepare(db, "DELETE FROM message WHERE id = ?1;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, id);
        int rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        return rc == AIC_DONE ? AIC_OK : rc;
    }

    char cid[64];
    aic_strlcpy_utf8(cid, sizeof(cid), jv_get_str(p, "conversation_id", ""));
    if (!cid[0]) {
        if (err && errlen) snprintf(err, errlen, "消息事件缺少 conversation_id");
        return AIC_E_FORMAT;
    }

    /* 会话合并：远端 chat 会话并到本地 chat 会话。 */
    char lc[64];
    if (local_chat_id(db, NULL, lc, sizeof(lc))) {
        aic_stmt *q = aic_db_prepare(db, "SELECT mode FROM conversation WHERE id = ?1;");
        int is_chat = 0;
        if (q) {
            aic_stmt_bind_text(q, 1, cid);
            if (aic_stmt_step(q) == AIC_ROW) {
                const char *m = aic_stmt_text(q, 0);
                is_chat = m && strcmp(m, "chat") == 0;
            }
            aic_stmt_finalize(q);
        }
        if (is_chat && strcmp(cid, lc) != 0) aic_strlcpy_utf8(cid, sizeof(cid), lc);
    }

    /* 会话必须在本地存在，否则建一个占位会话避免消息孤立。 */
    aic_stmt *cs = aic_db_prepare(db, "SELECT 1 FROM conversation WHERE id = ?1;");
    int have_conv = 0;
    if (cs) {
        aic_stmt_bind_text(cs, 1, cid);
        have_conv = (aic_stmt_step(cs) == AIC_ROW);
        aic_stmt_finalize(cs);
    }
    if (!have_conv) {
        aic_stmt *ci = aic_db_prepare(db,
            "INSERT OR IGNORE INTO conversation(id, mode, title, persona_id, model_id, pinned,"
            " last_seq, created_at, updated_at) VALUES(?1, 'qa', ?2, '', ?3, 0, 0, ?4, ?4);");
        if (!ci) return AIC_E_DB;
        char now[32];
        aic_now_iso8601(now);
        const char *mode = jv_get_str(p, "mode", "qa");
        aic_stmt_bind_text(ci, 1, cid);
        aic_stmt_bind_text(ci, 2, jv_get_str(p, "title", "来自另一台设备的会话"));
        aic_stmt_bind_text(ci, 3, jv_get_str(p, "model_id", AIC_MODEL_FLASH));
        aic_stmt_bind_text(ci, 4, now);
        aic_stmt_step(ci);
        aic_stmt_finalize(ci);
        (void)mode;
    }

    long long seq = jv_get_ll(p, "seq", 0);
    /* seq 被同一会话内另一条消息占用时顺延到末尾，避免 UNIQUE(conversation_id, seq) 冲突。 */
    aic_stmt *sc = aic_db_prepare(db,
        "SELECT 1 FROM message WHERE conversation_id = ?1 AND seq = ?2 AND id <> ?3;");
    if (sc) {
        aic_stmt_bind_text(sc, 1, cid);
        aic_stmt_bind_ll(sc, 2, seq);
        aic_stmt_bind_text(sc, 3, id);
        int taken = (aic_stmt_step(sc) == AIC_ROW);
        aic_stmt_finalize(sc);
        if (taken || seq <= 0) {
            aic_stmt *mx = aic_db_prepare(db,
                "SELECT COALESCE(MAX(seq), 0) + 1 FROM message WHERE conversation_id = ?1;");
            if (mx) {
                aic_stmt_bind_text(mx, 1, cid);
                if (aic_stmt_step(mx) == AIC_ROW) seq = aic_stmt_ll(mx, 0);
                aic_stmt_finalize(mx);
            }
        }
    }

    const char *role = jv_get_str(p, "role", "user");
    const char *content = jv_get_str(p, "content", "");
    const char *created = jv_get_str(p, "created_at", "");
    char now[32];
    if (!created[0]) {
        aic_now_iso8601(now);
        created = now;
    }

    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO message(id, conversation_id, seq, role, content, recalled, edited_at,"
        " original_content, model_id, created_at, superseded, logical_ts, origin_device)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)"
        " ON CONFLICT(id) DO UPDATE SET seq = excluded.seq, content = excluded.content,"
        " recalled = excluded.recalled, edited_at = excluded.edited_at,"
        " original_content = excluded.original_content, model_id = excluded.model_id,"
        " superseded = excluded.superseded, logical_ts = excluded.logical_ts,"
        " origin_device = excluded.origin_device;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, cid);
    aic_stmt_bind_ll(st, 3, seq);
    aic_stmt_bind_text(st, 4, role);
    aic_stmt_bind_text(st, 5, content);
    aic_stmt_bind_ll(st, 6, jv_get_ll(p, "recalled", 0));
    if (jv_get_str(p, "edited_at", NULL)) aic_stmt_bind_text(st, 7, jv_get_str(p, "edited_at", ""));
    else aic_stmt_bind_null(st, 7);
    if (jv_get_str(p, "original_content", NULL)) {
        aic_stmt_bind_text(st, 8, jv_get_str(p, "original_content", ""));
    } else {
        aic_stmt_bind_null(st, 8);
    }
    if (jv_get_str(p, "model_id", NULL)) aic_stmt_bind_text(st, 9, jv_get_str(p, "model_id", ""));
    else aic_stmt_bind_null(st, 9);
    aic_stmt_bind_text(st, 10, created);
    aic_stmt_bind_ll(st, 11, jv_get_ll(p, "superseded", 0));
    aic_stmt_bind_ll(st, 12, ev->logical_ts);
    aic_stmt_bind_text(st, 13, ev->origin_device);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) {
        if (err && errlen) snprintf(err, errlen, "写入远端消息失败：%s", aic_strerror(rc));
        return rc;
    }

    aic_stmt *up = aic_db_prepare(db,
        "UPDATE conversation SET last_seq = MAX(last_seq, ?2), updated_at = ?3 WHERE id = ?1;");
    if (up) {
        aic_stmt_bind_text(up, 1, cid);
        aic_stmt_bind_ll(up, 2, seq);
        aic_stmt_bind_text(up, 3, created);
        aic_stmt_step(up);
        aic_stmt_finalize(up);
    }
    return AIC_OK;
}

static int apply_conversation(aic_db *db, const aic_sync_event *ev, const jv *p,
                              char *err, size_t errlen)
{
    const char *id = jv_get_str(p, "id", ev->entity_id);
    if (!id || !*id) {
        if (err && errlen) snprintf(err, errlen, "会话事件缺少 id");
        return AIC_E_FORMAT;
    }
    if (aic_sync_op_parse(ev->op) == AIC_OP_DELETE) {
        aic_stmt *st = aic_db_prepare(db, "DELETE FROM conversation WHERE id = ?1 AND mode <> 'chat';");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, id);
        int rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        return rc == AIC_DONE ? AIC_OK : rc;
    }
    const char *mode = jv_get_str(p, "mode", "qa");
    char lc[64];
    if (strcmp(mode, "chat") == 0 && local_chat_id(db, id, lc, sizeof(lc))) {
        /* 本地已有另一个 chat 会话：把远端的消息并过来，不新建会话（FR-14 单对话）。 */
        aic_stmt *mv = aic_db_prepare(db,
            "UPDATE message SET conversation_id = ?2 WHERE conversation_id = ?1;");
        if (mv) {
            aic_stmt_bind_text(mv, 1, id);
            aic_stmt_bind_text(mv, 2, lc);
            aic_stmt_step(mv);
            aic_stmt_finalize(mv);
        }
        return AIC_OK;
    }
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO conversation(id, mode, title, persona_id, model_id, pinned, last_seq,"
        " created_at, updated_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
        " ON CONFLICT(id) DO UPDATE SET title = excluded.title, persona_id = excluded.persona_id,"
        " model_id = excluded.model_id, pinned = excluded.pinned,"
        " last_seq = MAX(conversation.last_seq, excluded.last_seq),"
        " updated_at = excluded.updated_at;");
    if (!st) return AIC_E_DB;
    char now[32];
    aic_now_iso8601(now);
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, mode);
    aic_stmt_bind_text(st, 3, jv_get_str(p, "title", ""));
    aic_stmt_bind_text(st, 4, jv_get_str(p, "persona_id", ""));
    aic_stmt_bind_text(st, 5, jv_get_str(p, "model_id", AIC_MODEL_FLASH));
    aic_stmt_bind_ll(st, 6, jv_get_ll(p, "pinned", 0));
    aic_stmt_bind_ll(st, 7, jv_get_ll(p, "last_seq", 0));
    aic_stmt_bind_text(st, 8, jv_get_str(p, "created_at", now));
    aic_stmt_bind_text(st, 9, jv_get_str(p, "updated_at", now));
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int apply_setting(aic_db *db, const aic_sync_event *ev, const jv *p,
                         char *err, size_t errlen)
{
    const char *key = jv_get_str(p, "key", ev->entity_id);
    if (!key || !*key) {
        if (err && errlen) snprintf(err, errlen, "设置事件缺少 key");
        return AIC_E_FORMAT;
    }
    if (!setting_syncable(key)) {
        if (err && errlen) snprintf(err, errlen, "设置项 %s 不在同步范围内", key);
        return AIC_E_DISABLED;
    }
    if (aic_sync_op_parse(ev->op) == AIC_OP_DELETE) {
        aic_stmt *st = aic_db_prepare(db, "DELETE FROM setting WHERE key = ?1;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, key);
        int rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        return rc == AIC_DONE ? AIC_OK : rc;
    }
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO setting(key, value) VALUES(?1, ?2)"
        " ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, key);
    aic_stmt_bind_text(st, 2, jv_get_str(p, "value", ""));
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int apply_profile(aic_db *db, const jv *p)
{
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO user_profile(id, name, avatar_path, gender, birthday, occupation, extra_note, updated_at)"
        " VALUES(1, ?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT(id) DO UPDATE SET name = excluded.name, avatar_path = excluded.avatar_path,"
        " gender = excluded.gender, birthday = excluded.birthday, occupation = excluded.occupation,"
        " extra_note = excluded.extra_note, updated_at = excluded.updated_at;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, jv_get_str(p, "name", ""));
    aic_stmt_bind_text(st, 2, jv_get_str(p, "avatar_path", ""));
    aic_stmt_bind_text(st, 3, jv_get_str(p, "gender", "未设置"));
    aic_stmt_bind_text(st, 4, jv_get_str(p, "birthday", ""));
    aic_stmt_bind_text(st, 5, jv_get_str(p, "occupation", ""));
    aic_stmt_bind_text(st, 6, jv_get_str(p, "extra_note", ""));
    aic_stmt_bind_text(st, 7, jv_get_str(p, "updated_at", now));
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int apply_persona(aic_db *db, const aic_sync_event *ev, const jv *p,
                         char *err, size_t errlen)
{
    const char *id = jv_get_str(p, "id", ev->entity_id);
    if (!id || !*id) {
        if (err && errlen) snprintf(err, errlen, "人设事件缺少 id");
        return AIC_E_FORMAT;
    }
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO persona(id, name, description, base_prompt, current_version, created_at, updated_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT(id) DO UPDATE SET name = excluded.name, description = excluded.description,"
        " base_prompt = excluded.base_prompt, current_version = excluded.current_version,"
        " updated_at = excluded.updated_at;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, jv_get_str(p, "name", ""));
    aic_stmt_bind_text(st, 3, jv_get_str(p, "description", ""));
    aic_stmt_bind_text(st, 4, jv_get_str(p, "base_prompt", ""));
    aic_stmt_bind_ll(st, 5, jv_get_ll(p, "current_version", 1));
    aic_stmt_bind_text(st, 6, jv_get_str(p, "created_at", now));
    aic_stmt_bind_text(st, 7, jv_get_str(p, "updated_at", now));
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int apply_prompt_version(aic_db *db, const aic_sync_event *ev, const jv *p,
                                char *err, size_t errlen)
{
    const char *pid = jv_get_str(p, "persona_id", "");
    if (!pid[0]) {
        if (err && errlen) snprintf(err, errlen, "提示词版本事件缺少 persona_id");
        return AIC_E_FORMAT;
    }
    char id[64];
    aic_strlcpy_utf8(id, sizeof(id), jv_get_str(p, "id", ev->entity_id));
    if (!id[0]) aic_uuid4(id);
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO prompt_version(id, persona_id, version, content, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5)"
        " ON CONFLICT(persona_id, version) DO UPDATE SET content = excluded.content;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, pid);
    aic_stmt_bind_ll(st, 3, jv_get_ll(p, "version", 1));
    aic_stmt_bind_text(st, 4, jv_get_str(p, "content", ""));
    aic_stmt_bind_text(st, 5, jv_get_str(p, "created_at", now));
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int apply_plugin(aic_db *db, const aic_sync_event *ev, const jv *p,
                        char *err, size_t errlen)
{
    const char *id = jv_get_str(p, "id", ev->entity_id);
    if (!id || !*id) {
        if (err && errlen) snprintf(err, errlen, "插件事件缺少 id");
        return AIC_E_FORMAT;
    }
    if (aic_sync_op_parse(ev->op) == AIC_OP_DELETE) {
        aic_stmt *st = aic_db_prepare(db, "DELETE FROM plugin WHERE id = ?1;");
        if (!st) return AIC_E_DB;
        aic_stmt_bind_text(st, 1, id);
        int rc = aic_stmt_step(st);
        aic_stmt_finalize(st);
        return rc == AIC_DONE ? AIC_OK : rc;
    }
    char now[32];
    aic_now_iso8601(now);
    /* 插件二进制不参与同步：清单照收，但一律置为未启用，避免指向不存在的动态库。 */
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO plugin(id, name, version, author, entry_path, permissions, extension_points,"
        " import_source, enabled, installed_at, data_dir)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 0, ?9, ?10)"
        " ON CONFLICT(id) DO UPDATE SET name = excluded.name, version = excluded.version,"
        " author = excluded.author, entry_path = excluded.entry_path,"
        " permissions = excluded.permissions, extension_points = excluded.extension_points,"
        " import_source = excluded.import_source, data_dir = excluded.data_dir;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, id);
    aic_stmt_bind_text(st, 2, jv_get_str(p, "name", id));
    aic_stmt_bind_text(st, 3, jv_get_str(p, "version", "0.0.0"));
    aic_stmt_bind_text(st, 4, jv_get_str(p, "author", ""));
    aic_stmt_bind_text(st, 5, jv_get_str(p, "entry_path", ""));
    aic_stmt_bind_text(st, 6, jv_get_str(p, "permissions", ""));
    aic_stmt_bind_text(st, 7, jv_get_str(p, "extension_points", ""));
    aic_stmt_bind_text(st, 8, jv_get_str(p, "import_source", "file"));
    aic_stmt_bind_text(st, 9, jv_get_str(p, "installed_at", now));
    aic_stmt_bind_text(st, 10, jv_get_str(p, "data_dir", ""));
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

static int apply_payload(aic_sync *s, const aic_sync_event *ev, char *err, size_t errlen)
{
    jv *p = NULL;
    char jerr[256];
    jerr[0] = '\0';
    int rc = jv_parse(ev->payload ? ev->payload : "{}",
                      ev->payload ? strlen(ev->payload) : 2, &p, jerr, sizeof(jerr));
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "事件 payload 不是合法 JSON：%s", jerr);
        return AIC_E_FORMAT;
    }
    int ent = aic_sync_entity_parse(ev->entity);
    switch (ent) {
    case AIC_ENT_MESSAGE:      rc = apply_message(s->db, ev, p, err, errlen); break;
    case AIC_ENT_CONVERSATION: rc = apply_conversation(s->db, ev, p, err, errlen); break;
    case AIC_ENT_SETTING:      rc = apply_setting(s->db, ev, p, err, errlen); break;
    case AIC_ENT_PROFILE:      rc = apply_profile(s->db, p); break;
    case AIC_ENT_PERSONA:      rc = apply_persona(s->db, ev, p, err, errlen); break;
    case AIC_ENT_PROMPT_VERSION: rc = apply_prompt_version(s->db, ev, p, err, errlen); break;
    case AIC_ENT_PLUGIN:       rc = apply_plugin(s->db, ev, p, err, errlen); break;
    default:
        if (err && errlen) snprintf(err, errlen, "不支持的同步实体：%s", ev->entity);
        rc = AIC_E_INVAL;
        break;
    }
    jv_free(p);
    return rc;
}

/* 已处理过的入站事件？（按 event_id 去重） */
static int inbound_seen(aic_db *db, const char *event_id)
{
    aic_stmt *st = aic_db_prepare(db,
        "SELECT 1 FROM sync_event WHERE event_id = ?1 AND direction = 'in';");
    if (!st) return 0;
    aic_stmt_bind_text(st, 1, event_id);
    int seen = (aic_stmt_step(st) == AIC_ROW);
    aic_stmt_finalize(st);
    return seen;
}

static int record_inbound(aic_db *db, const aic_sync_event *ev, const char *state)
{
    aic_stmt *st = aic_db_prepare(db,
        "INSERT OR IGNORE INTO sync_event(event_id, entity, op, entity_id, payload, logical_ts,"
        " origin_device, schema_version, direction, state, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'in', ?9, ?10);");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, ev->event_id);
    aic_stmt_bind_text(st, 2, ev->entity);
    aic_stmt_bind_text(st, 3, ev->op);
    aic_stmt_bind_text(st, 4, ev->entity_id);
    aic_stmt_bind_text(st, 5, ev->payload ? ev->payload : "{}");
    aic_stmt_bind_ll(st, 6, ev->logical_ts);
    aic_stmt_bind_text(st, 7, ev->origin_device);
    aic_stmt_bind_ll(st, 8, ev->schema_version ? ev->schema_version : AIC_SYNC_SCHEMA_VERSION);
    aic_stmt_bind_text(st, 9, state);
    aic_stmt_bind_ll(st, 10, aic_now_ms());
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_sync_apply_event(aic_sync *s, const aic_sync_event *ev, int *out_applied,
                         char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_applied) *out_applied = 0;
    if (!s || !ev) return AIC_E_INVAL;
    if (!aic_uuid_valid(ev->event_id)) {
        if (err && errlen) snprintf(err, errlen, "事件 event_id 不是合法 UUID");
        return AIC_E_FORMAT;
    }
    if (inbound_seen(s->db, ev->event_id)) return AIC_OK; /* 重复事件直接忽略 */

    /* 远端逻辑时钟必须抬高本端已知最大值，保证后续本地新增不会更小。 */
    int rc = aic_clock_observe(s->db, ev->logical_ts);
    if (rc != AIC_OK) return rc;

    /* §16.6 冲突判定：(logical_ts, origin_device) 较大者胜；相等按设备字典序。 */
    long long local_ts = 0;
    char local_dev[AIC_SYNC_DEVICE_MAX];
    rc = entity_clock_get(s->db, ev->entity, ev->entity_id, &local_ts, local_dev, sizeof(local_dev));
    if (rc != AIC_OK) return rc;

    int lose = 0;
    if (local_ts > 0) {
        if (ev->logical_ts < local_ts) lose = 1;
        else if (ev->logical_ts == local_ts && strcmp(ev->origin_device, local_dev) <= 0) lose = 1;
    }
    if (lose) {
        save_conflict(s->db, ev);
        record_inbound(s->db, ev, "conflict");
        cursor_advance(s->db, ev->origin_device, ev->event_id, ev->logical_ts);
        if (out_applied) *out_applied = 0;
        return AIC_OK;
    }

    rc = apply_payload(s, ev, err, errlen);
    if (rc != AIC_OK) {
        if (rc == AIC_E_DISABLED) return rc; /* 不在同步范围内，静默跳过由调用方决定 */
        return rc;
    }
    entity_clock_set(s->db, ev->entity, ev->entity_id, ev->logical_ts, ev->origin_device);
    record_inbound(s->db, ev, "applied");
    cursor_advance(s->db, ev->origin_device, ev->event_id, ev->logical_ts);
    if (out_applied) *out_applied = 1;
    return AIC_OK;
}

int aic_sync_apply_json(aic_sync *s, const char *text, size_t len, int *out_applied,
                        char *err, size_t errlen)
{
    if (out_applied) *out_applied = 0;
    aic_sync_event ev;
    int rc = aic_sync_event_from_json(text, len, &ev, err, errlen);
    if (rc != AIC_OK) return rc;
    rc = aic_sync_apply_event(s, &ev, out_applied, err, errlen);
    aic_sync_event_free(&ev);
    return rc;
}
