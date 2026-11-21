/* aic_jni.c —— Android 端桥接层。
 *
 * 界面层（Java）不直接调用几十个核心函数，而是通过单一入口 call(json) 交换 JSON：
 *   request : {"cmd":"state"} 等
 *   response: {"ok":true,"err":"",...}
 * 这样核心层（src/core）在两端完全共用，Android 侧只多一层薄适配。
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <android/log.h>

#include "aic_types.h"
#include "util.h"
#include "db.h"
#include "json.h"
#include "llm.h"
#include "engine.h"
#include "router.h"
#include "theme.h"
#include "prompt.h"
#include "transfer.h"
#include "plugin.h"
#include "perf.h"
#include "store_profile.h"
#include "store_persona.h"
#include "store_conversation.h"
#include "store_message.h"
#include "store_setting.h"
#include "store_usage.h"
#include "store_guide.h"

#define TAG "AIChatCore"
#define JLOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static aic_db     *g_db;
static aic_engine *g_eng;
static aic_router *g_router;
static char        g_dir[400];       /* 应用私有根目录 */
static char        g_conv[40];       /* 当前查看的会话 id（问答线程或唯一聊天会话） */
static char        g_err[512];

/* ---------- 离线演示传输 ----------
 * 与桌面端一致：不发真实 HTTP，只把固定回复按 SSE 分片喂给解析器，
 * 这样"发送 / 流式增量 / 落库 / 用量记账"整条链路都能在没有网络时验证。 */
static void demo_reply(const char *body, char out1[400], char out2[400], char out3[400])
{
    const char *user = body ? body : "";
    int warm = (int)strlen(user) % 2;
    if (warm) {
        snprintf(out1, 400, "我在听。");
        snprintf(out2, 400, "你刚才说的这些，听起来确实攒了一阵子了——");
        snprintf(out3, 400, "要不要先挑最让你睡不着的那一件，我们一起拆开看看？");
    } else {
        snprintf(out1, 400, "嗯，我理解。");
        snprintf(out2, 400, "先别急着把所有事都想清楚，");
        snprintf(out3, 400, "一次只处理一件，剩下的我陪着你慢慢说。");
    }
}

static aic_llm_err demo_post(void *ud, const char *url, const char *api_key, const char *body,
                             aic_sse_parser *parser, aic_llm_event_fn fn, void *cb_ud,
                             char *err, size_t errlen)
{
    char p1[400], p2[400], p3[400];
    char *parts[3];
    sb chunk;
    int i;

    (void)ud; (void)url;
    if (!api_key || !api_key[0]) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "尚未配置 API Key，请先到设置里填写");
        return AIC_LLM_E_AUTH;
    }
    demo_reply(body, p1, p2, p3);
    parts[0] = p1; parts[1] = p2; parts[2] = p3;
    sb_init(&chunk);
    for (i = 0; i < 3; i++) {
        sb_reset(&chunk);
        sb_append(&chunk, "data: {\"choices\":[{\"delta\":{\"content\":");
        jw_escape_into(&chunk, parts[i]);
        sb_append(&chunk, "}}]}\n\n");
        aic_sse_feed(parser, sb_cstr(&chunk), chunk.len, fn, cb_ud);
    }
    sb_reset(&chunk);
    sb_appendf(&chunk, "data: {\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d}}\n\n",
               48 + (int)(strlen(body ? body : "") / 3), 32);
    aic_sse_feed(parser, sb_cstr(&chunk), chunk.len, fn, cb_ud);
    aic_sse_feed(parser, "data: [DONE]\n\n", 14, fn, cb_ud);
    aic_sse_finish(parser, fn, cb_ud);
    sb_free(&chunk);
    return AIC_LLM_OK;
}

static aic_llm_err demo_probe(void *ud, const char *url, const char *api_key,
                              char *err, size_t errlen)
{
    (void)ud; (void)url;
    if (!api_key || !api_key[0]) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "尚未配置 API Key");
        return AIC_LLM_E_AUTH;
    }
    return AIC_LLM_OK;
}

static aic_llm_transport demo_transport(void)
{
    aic_llm_transport tr;
    memset(&tr, 0, sizeof(tr));
    tr.ud = NULL;
    tr.post_stream = demo_post;
    tr.probe = demo_probe;
    return tr;
}

/* ---------- 初始化 ---------- */
static int ensure_env(void)
{
    char p[512], sub[512];
    int rc;
    const char *subs[5] = { "db", "plugins", "avatars", "backups", "exports" };
    int i;

    if (g_db) return AIC_OK;
    if (!g_dir[0]) { aic_strlcpy_utf8(g_err, sizeof(g_err), "尚未调用 init"); return AIC_E_STATE; }
    for (i = 0; i < 5; i++) {
        aic_path_join(sub, sizeof(sub), g_dir, subs[i]);
        aic_mkdir_p(sub);
    }
    aic_path_join(p, sizeof(p), g_dir, "db");
    aic_path_join(sub, sizeof(sub), p, "aic.db");
    rc = aic_db_open(sub, &g_db);
    if (rc != AIC_OK) {
        snprintf(g_err, sizeof(g_err), "数据库打开失败（%d）", rc);
        return rc;
    }
    aic_path_join(p, sizeof(p), g_dir, "backups");
    rc = aic_db_migrate(g_db, sub, p, NULL, NULL);
    if (rc != AIC_OK) {
        snprintf(g_err, sizeof(g_err), "数据库迁移失败（%d）", rc);
        return rc;
    }
    rc = aic_router_create(g_db, &g_router);
    if (rc != AIC_OK) return rc;
    rc = aic_engine_create(g_db, &g_eng);
    if (rc != AIC_OK) return rc;
    aic_engine_set_transport(g_eng, demo_transport());
    aic_engine_set_context_messages(g_eng, aic_setting_get_int(g_db, AIC_SET_CONTEXT_N,
                                                              AIC_CONTEXT_MESSAGES_DEFAULT));
    aic_router_boot(g_router);
    aic_router_config_check(g_router);
    return AIC_OK;
}

/* 聊天模式取唯一会话；问答模式取当前线程或最近一条。 */
static const char *current_conv(void)
{
    aic_conversation c;
    size_t n = 0;
    aic_conversation *arr = NULL;

    if (!g_db) return "";
    if (aic_router_mode(g_router) == AIC_MODE_CHAT) {
        if (aic_conv_get_chat(g_db, &c) == AIC_OK) {
            aic_strlcpy_utf8(g_conv, sizeof(g_conv), c.id);
            return g_conv;
        }
        if (aic_conv_ensure_chat(g_db, AIC_MODEL_FLASH, "", &c) == AIC_OK) {
            aic_strlcpy_utf8(g_conv, sizeof(g_conv), c.id);
            return g_conv;
        }
        return "";
    }
    if (g_conv[0]) return g_conv;
    if (aic_conv_list(g_db, AIC_MODE_QA, &arr, &n) == AIC_OK && n > 0) {
        aic_strlcpy_utf8(g_conv, sizeof(g_conv), arr[0].id);
        aic_conv_free(arr, n);
        return g_conv;
    }
    if (arr) aic_conv_free(arr, n);
    return "";
}

/* ---------- JSON 组装 ---------- */
static void put_theme(jw *w)
{
    int pref = aic_setting_get_int(g_db, AIC_SET_THEME, 0);
    const aic_theme *th = aic_theme_resolve(pref, 0);
    int i;

    jw_obj_begin(w, "theme");
    jw_put_str(w, "pref", pref == 1 ? "night" : (pref == 2 ? "system" : "day"));
    jw_put_str(w, "name", th->name);
    jw_put_bool(w, "night", th->night);
    jw_obj_begin(w, "tokens");
    for (i = 0; i < AIC_TOK_COUNT; i++) {
        char hex[16];
        snprintf(hex, sizeof(hex), "#%06X", aic_theme_color(th, (aic_token)i) & 0xFFFFFFu);
        jw_put_str(w, aic_token_name((aic_token)i), hex);
    }
    jw_obj_end(w);
    jw_obj_end(w);
}

static void put_motion(jw *w)
{
    aic_motion m = aic_motion_default();
    if (aic_setting_get_int(g_db, AIC_SET_MOTION, 1) == 0) m = aic_motion_reduced();
    jw_obj_begin(w, "motion");
    jw_put_int(w, "modeSwitch", m.mode_switch_ms);
    jw_put_int(w, "tabSwitch", m.tab_switch_ms);
    jw_put_int(w, "subpage", m.subpage_ms);
    jw_put_int(w, "overlay", m.overlay_ms);
    jw_put_int(w, "message", m.message_ms);
    jw_put_int(w, "toast", m.toast_ms);
    jw_put_bool(w, "reduced", m.reduced);
    jw_obj_end(w);
}

static void put_profile(jw *w)
{
    aic_profile p;
    memset(&p, 0, sizeof(p));
    aic_profile_get(g_db, &p);
    jw_obj_begin(w, "profile");
    jw_put_str(w, "name", p.name);
    jw_put_str(w, "gender", p.gender);
    jw_put_str(w, "birthday", p.birthday);
    jw_put_str(w, "occupation", p.occupation);
    jw_put_str(w, "extraNote", p.extra_note);
    jw_put_str(w, "avatarPath", p.avatar_path);
    jw_obj_end(w);
}

static void put_persona(jw *w)
{
    aic_persona per;
    memset(&per, 0, sizeof(per));
    if (aic_persona_ensure_default(g_db, &per) != AIC_OK) {
        jw_put_null(w, "persona");
        return;
    }
    jw_obj_begin(w, "persona");
    jw_put_str(w, "id", per.id);
    jw_put_str(w, "name", per.name);
    jw_put_str(w, "description", per.description);
    jw_put_str(w, "prompt", per.base_prompt ? per.base_prompt : "");
    jw_put_int(w, "version", per.current_version);
    jw_obj_end(w);
    aic_persona_free(&per);
}

static void put_msgs(jw *w, const char *conv_id)
{
    aic_message *arr = NULL;
    size_t n = 0, i;

    jw_arr_begin(w, "msgs");
    if (conv_id && conv_id[0] && aic_msg_export_all(g_db, conv_id, &arr, &n) == AIC_OK) {
        for (i = 0; i < n; i++) {
            jw_obj_begin(w, NULL);
            jw_put_str(w, "id", arr[i].id);
            jw_put_int(w, "seq", arr[i].seq);
            jw_put_str(w, "role", arr[i].role);
            jw_put_str(w, "content", arr[i].content ? arr[i].content : "");
            jw_put_bool(w, "recalled", arr[i].recalled);
            jw_put_bool(w, "edited", arr[i].edited_at[0] != 0);
            jw_put_str(w, "ts", arr[i].created_at);
            jw_obj_end(w);
        }
        aic_msg_free_array(arr, n);
    }
    jw_arr_end(w);
}

static void put_threads(jw *w)
{
    aic_conversation *arr = NULL;
    size_t n = 0, i;

    jw_arr_begin(w, "threads");
    if (aic_conv_list(g_db, -1, &arr, &n) == AIC_OK) {
        for (i = 0; i < n; i++) {
            jw_obj_begin(w, NULL);
            jw_put_str(w, "id", arr[i].id);
            jw_put_str(w, "title", arr[i].title);
            jw_put_str(w, "mode", arr[i].mode);
            jw_put_bool(w, "pinned", arr[i].pinned);
            jw_put_int(w, "lastSeq", arr[i].last_seq);
            jw_put_str(w, "updatedAt", arr[i].updated_at);
            jw_obj_end(w);
        }
        aic_conv_free(arr, n);
    }
    jw_arr_end(w);
}

static void put_guide(jw *w)
{
    aic_guide_state gs;
    const aic_guide_step *steps = aic_guide_steps();
    int i;

    memset(&gs, 0, sizeof(gs));
    aic_guide_state_get(g_db, &gs);
    jw_obj_begin(w, "guide");
    jw_put_int(w, "completed", gs.completed_count);
    jw_put_int(w, "count", AIC_GUIDE_STEP_COUNT);
    jw_put_int(w, "next", gs.next_index);
    jw_arr_begin(w, "steps");
    for (i = 0; i < AIC_GUIDE_STEP_COUNT; i++) {
        jw_obj_begin(w, NULL);
        jw_put_str(w, "id", steps[i].id);
        jw_put_str(w, "title", steps[i].title);
        jw_put_str(w, "goal", steps[i].goal);
        jw_put_str(w, "doneWhen", steps[i].done_when);
        jw_put_bool(w, "done", gs.completed[i] && !gs.skipped[i]);
        jw_put_bool(w, "skipped", gs.skipped[i]);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}

static void put_usage(jw *w)
{
    aic_usage_summary sum;
    char money[32];
    memset(&sum, 0, sizeof(sum));
    aic_usage_summary_get(g_db, 0, &sum);
    aic_llm_format_micros(sum.cost_micros, money, sizeof(money));
    jw_obj_begin(w, "usage");
    jw_put_int(w, "requests", sum.requests);
    jw_put_int(w, "promptTokens", sum.prompt_tokens);
    jw_put_int(w, "completionTokens", sum.completion_tokens);
    jw_put_int(w, "totalTokens", sum.total_tokens);
    jw_put_int(w, "costMicros", sum.cost_micros);
    jw_put_str(w, "cost", money);
    jw_obj_end(w);
}

static void put_models(jw *w)
{
    size_t i, n = aic_llm_model_count();
    jw_arr_begin(w, "models");
    for (i = 0; i < n; i++) {
        const aic_llm_model *m = aic_llm_model_at(i);
        jw_obj_begin(w, NULL);
        jw_put_str(w, "id", m->id);
        jw_put_str(w, "name", m->name);
        jw_put_str(w, "summary", m->summary);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

static void put_plugins(jw *w)
{
    aic_plugin *arr = NULL;
    size_t n = 0, i, on = 0;
    if (aic_plugin_list(g_db, -1, &arr, &n) == AIC_OK) {
        for (i = 0; i < n; i++) if (arr[i].enabled) on++;
    }
    jw_obj_begin(w, "plugins");
    jw_put_int(w, "total", (long long)n);
    jw_put_int(w, "enabled", (long long)on);
    jw_obj_end(w);
    if (arr) aic_plugin_free_array(arr, n);
}

static void put_conv(jw *w)
{
    const char *id = current_conv();
    aic_conversation c;
    memset(&c, 0, sizeof(c));
    jw_obj_begin(w, "conv");
    if (id && id[0] && aic_conv_get(g_db, id, &c) == AIC_OK) {
        jw_put_str(w, "id", c.id);
        jw_put_str(w, "title", c.title);
        jw_put_str(w, "mode", c.mode);
        jw_put_str(w, "model", c.model_id);
    } else {
        jw_put_str(w, "id", "");
        jw_put_str(w, "title", "");
        jw_put_str(w, "mode", "");
        jw_put_str(w, "model", "");
    }
    jw_obj_end(w);
}

static void build_state(sb *out)
{
    jw w;
    aic_route_state st;
    int tab;
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "mode", aic_router_mode(g_router) == AIC_MODE_QA ? "qa" : "chat");
    jw_put_str(&w, "modeName", aic_router_mode(g_router) == AIC_MODE_QA ? "问答模式" : "聊天模式");
    tab = (int)aic_router_tab(g_router);
    jw_put_int(&w, "tab", tab);
    jw_put_str(&w, "tabName", aic_tab_name((aic_tab)tab));
    st = aic_router_state(g_router);
    jw_put_str(&w, "state", aic_route_state_name(st));
    jw_put_bool(&w, "switching", aic_router_is_switching(g_router));
    jw_put_int(&w, "overlay", (long long)aic_router_top_overlay(g_router));
    jw_put_str(&w, "overlayName", aic_overlay_name(aic_router_top_overlay(g_router)));
    jw_put_bool(&w, "hasKey", aic_engine_has_key(g_eng));
    jw_put_str(&w, "model", aic_setting_get_int(g_db, AIC_SET_MODEL_DEFAULT, 0) == 1
                                   ? AIC_MODEL_PRO : AIC_MODEL_FLASH);
    put_theme(&w);
    put_motion(&w);
    put_profile(&w);
    put_persona(&w);
    put_conv(&w);
    put_msgs(&w, current_conv());
    put_threads(&w);
    put_guide(&w);
    put_usage(&w);
    put_models(&w);
    put_plugins(&w);
    jw_obj_end(&w);
}

/* ---------- 命令处理 ---------- */
static void reply_ok(sb *out)
{
    jw w;
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_obj_end(&w);
}

static void reply_err(sb *out, const char *msg)
{
    jw w;
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 0);
    jw_put_str(&w, "err", msg ? msg : "未知错误");
    jw_obj_end(&w);
}

static int cmd_send(const char *text, sb *out)
{
    aic_engine_result res;
    char err[256] = { 0 };
    const char *conv = current_conv();
    jw w;
    int rc;

    if (!conv[0]) return reply_err(out, "没有可用的会话"), AIC_E_STATE;
    memset(&res, 0, sizeof(res));
    rc = aic_engine_send(g_eng, conv, text, &res, err, sizeof(err));
    if (rc != AIC_OK) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s", err[0] ? err : aic_strerror(rc));
        aic_engine_result_free(&res);
        reply_err(out, msg);
        return rc;
    }
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "text", res.text ? res.text : "");
    jw_put_int(&w, "promptTokens", res.prompt_tokens);
    jw_put_int(&w, "completionTokens", res.completion_tokens);
    jw_put_int(&w, "costMicros", res.cost_micros);
    jw_obj_end(&w);
    aic_engine_result_free(&res);
    return AIC_OK;
}

static int cmd_export(const char *kind, sb *out)
{
    char dir[512], path[512], name[128], err[256] = { 0 };
    int rc;

    aic_path_join(dir, sizeof(dir), g_dir, "exports");
    aic_mkdir_p(dir);
    snprintf(name, sizeof(name), "chat_record_%lld.%s",
             (long long)(aic_now_ms() / 1000), strcmp(kind, "md") == 0 ? "md"
                     : (strcmp(kind, "txt") == 0 ? "txt" : "json"));
    aic_path_join(path, sizeof(path), dir, name);
    if (strcmp(kind, "md") == 0) rc = aic_export_write_markdown(g_db, -1, path, err, sizeof(err));
    else if (strcmp(kind, "txt") == 0) rc = aic_export_write_text(g_db, -1, path, err, sizeof(err));
    else rc = aic_export_write_json(g_db, -1, path, err, sizeof(err));
    if (rc != AIC_OK) {
        reply_err(out, err[0] ? err : aic_strerror(rc));
        return rc;
    }
    {
        jw w;
        sb_reset(out);
        jw_init(&w, out, 0);
        jw_obj_begin(&w, NULL);
        jw_put_bool(&w, "ok", 1);
        jw_put_str(&w, "err", "");
        jw_put_str(&w, "path", path);
        jw_obj_end(&w);
    }
    return AIC_OK;
}

static int cmd_import(const char *path, int policy, sb *out)
{
    aic_import_preview pv;
    aic_import_result ir;
    char err[256] = { 0 };
    int rc;
    jw w;

    memset(&pv, 0, sizeof(pv));
    rc = aic_transfer_preview(g_db, path, &pv, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "导入文件校验失败"); return rc; }
    memset(&ir, 0, sizeof(ir));
    rc = aic_transfer_import(g_db, path, (aic_conflict_policy)policy, &ir, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "导入失败并已回滚"); return rc; }
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_int(&w, "convAdded", (long long)ir.conv_added);
    jw_put_int(&w, "msgAdded", (long long)ir.msg_added);
    jw_put_int(&w, "msgSkipped", (long long)ir.msg_skipped);
    jw_put_int(&w, "msgRenamed", (long long)ir.msg_renamed);
    jw_obj_end(&w);
    return AIC_OK;
}

static void handle(const char *req_text, size_t req_len, sb *out)
{
    jv *root = NULL;
    char perr[128] = { 0 };
    const char *cmd;
    int rc;

    g_err[0] = 0;
    if (jv_parse(req_text, req_len, &root, perr, sizeof(perr)) != AIC_OK) {
        reply_err(out, "请求不是合法 JSON");
        return;
    }
    cmd = jv_get_str(root, "cmd", "");
    (void)rc;

    if (strcmp(cmd, "init") == 0) {
        aic_strlcpy_utf8(g_dir, sizeof(g_dir), jv_get_str(root, "dir", ""));
        rc = ensure_env();
        if (rc != AIC_OK) reply_err(out, g_err[0] ? g_err : aic_strerror(rc));
        else reply_ok(out);
    } else if (strcmp(cmd, "state") == 0) {
        if (ensure_env() != AIC_OK) { reply_err(out, g_err); }
        else build_state(out);
    } else if (strcmp(cmd, "switchMode") == 0) {
        aic_mode target = strcmp(jv_get_str(root, "mode", "chat"), "qa") == 0
                          ? AIC_MODE_QA : AIC_MODE_CHAT;
        rc = aic_router_switch_mode(g_router, target);
        if (rc == AIC_E_BUSY) reply_err(out, "正在切换中，请稍候");
        else if (rc != AIC_OK) reply_err(out, aic_strerror(rc));
        else { aic_router_finish_switch(g_router); g_conv[0] = 0; reply_ok(out); }
    } else if (strcmp(cmd, "setTab") == 0) {
        rc = aic_router_set_tab(g_router, (aic_tab)jv_get_int(root, "tab", AIC_TAB_CHAT));
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "openThread") == 0) {
        const char *id = jv_get_str(root, "id", "");
        rc = aic_router_set_thread(g_router, id);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc));
        else { aic_strlcpy_utf8(g_conv, sizeof(g_conv), id); reply_ok(out); }
    } else if (strcmp(cmd, "closeThread") == 0) {
        aic_router_set_thread(g_router, "");
        g_conv[0] = 0;
        reply_ok(out);
    } else if (strcmp(cmd, "newThread") == 0) {
        aic_conversation c;
        char title[128];
        snprintf(title, sizeof(title), "%s", jv_get_str(root, "title", "新会话"));
        rc = aic_conv_create(g_db, AIC_MODE_QA, title, "", AIC_MODEL_FLASH, &c);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc));
        else {
            aic_strlcpy_utf8(g_conv, sizeof(g_conv), c.id);
            aic_router_set_thread(g_router, c.id);
            reply_ok(out);
        }
    } else if (strcmp(cmd, "send") == 0) {
        cmd_send(jv_get_str(root, "text", ""), out);
    } else if (strcmp(cmd, "recall") == 0) {
        char err[256] = { 0 };
        rc = aic_engine_recall(g_eng, jv_get_str(root, "id", ""),
                               aic_setting_get_int(g_db, AIC_SET_RECALL_WINDOW,
                                                   AIC_RECALL_WINDOW_DEFAULT), err, sizeof(err));
        if (rc != AIC_OK) reply_err(out, err[0] ? err : aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "edit") == 0) {
        char err[256] = { 0 };
        rc = aic_engine_edit(g_eng, jv_get_str(root, "id", ""), jv_get_str(root, "text", ""),
                             err, sizeof(err));
        if (rc != AIC_OK) reply_err(out, err[0] ? err : aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "setProfile") == 0) {
        aic_profile p;
        char err[256] = { 0 };
        memset(&p, 0, sizeof(p));
        aic_profile_get(g_db, &p);
        aic_strlcpy_utf8(p.name, sizeof(p.name), jv_get_str(root, "name", p.name));
        aic_strlcpy_utf8(p.gender, sizeof(p.gender), jv_get_str(root, "gender", p.gender));
        aic_strlcpy_utf8(p.birthday, sizeof(p.birthday), jv_get_str(root, "birthday", p.birthday));
        aic_strlcpy_utf8(p.occupation, sizeof(p.occupation),
                         jv_get_str(root, "occupation", p.occupation));
        aic_strlcpy_utf8(p.extra_note, sizeof(p.extra_note),
                         jv_get_str(root, "extraNote", p.extra_note));
        rc = aic_profile_set(g_db, &p, err, sizeof(err));
        if (rc != AIC_OK) reply_err(out, err[0] ? err : aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "setPersona") == 0) {
        aic_persona per, upd;
        char err[256] = { 0 };
        memset(&per, 0, sizeof(per));
        if (aic_persona_ensure_default(g_db, &per) != AIC_OK) { reply_err(out, "人设读取失败"); }
        else {
            memset(&upd, 0, sizeof(upd));
            rc = aic_persona_update(g_db, per.id,
                                    jv_get_str(root, "name", per.name),
                                    jv_get_str(root, "description", per.description),
                                    jv_get_str(root, "prompt", per.base_prompt ? per.base_prompt : ""),
                                    &upd);
            if (rc != AIC_OK) reply_err(out, err[0] ? err : aic_strerror(rc));
            else {
                jw w;
                sb_reset(out);
                jw_init(&w, out, 0);
                jw_obj_begin(&w, NULL);
                jw_put_bool(&w, "ok", 1);
                jw_put_str(&w, "err", "");
                jw_put_int(&w, "version", upd.current_version);
                jw_obj_end(&w);
                aic_persona_free(&upd);
            }
            aic_persona_free(&per);
        }
    } else if (strcmp(cmd, "guideMark") == 0) {
        rc = aic_guide_mark(g_db, jv_get_str(root, "step", ""), jv_get_bool(root, "skip", 0));
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "guideReset") == 0) {
        rc = aic_guide_reset(g_db);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "setTheme") == 0) {
        const char *pref = jv_get_str(root, "pref", "day");
        int v = strcmp(pref, "night") == 0 ? 1 : (strcmp(pref, "system") == 0 ? 2 : 0);
        rc = aic_setting_set_int(g_db, AIC_SET_THEME, v);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "setMotion") == 0) {
        rc = aic_setting_set_int(g_db, AIC_SET_MOTION, jv_get_bool(root, "on", 1) ? 1 : 0);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "setModel") == 0) {
        const char *id = jv_get_str(root, "id", AIC_MODEL_FLASH);
        if (!aic_llm_model_find(id)) reply_err(out, "未知模型");
        else {
            rc = aic_setting_set_int(g_db, AIC_SET_MODEL_DEFAULT,
                                     strcmp(id, AIC_MODEL_PRO) == 0 ? 1 : 0);
            if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
        }
    } else if (strcmp(cmd, "setKey") == 0) {
        /* Android 侧凭证保存需要系统安全存储（Keystore）；未接入前只在本次进程内使用。 */
        const char *key = jv_get_str(root, "key", "");
        aic_engine_set_api_key(g_eng, key);
        aic_setting_set(g_db, AIC_SET_CRED_HAS_KEY, (key && key[0]) ? "0" : "0");
        if (key && key[0]) reply_ok(out); else reply_err(out, "API Key 不能为空");
    } else if (strcmp(cmd, "probe") == 0) {
        char err[256] = { 0 };
        aic_llm_transport tr = demo_transport();
        aic_llm_err le = tr.probe(tr.ud, "https://api.deepseek.com/chat/completions",
                                  aic_engine_has_key(g_eng) ? "in-memory" : "", err, sizeof(err));
        if (le == AIC_LLM_OK) reply_ok(out);
        else reply_err(out, err[0] ? err : aic_llm_err_text(le));
    } else if (strcmp(cmd, "export") == 0) {
        cmd_export(jv_get_str(root, "kind", "json"), out);
    } else if (strcmp(cmd, "import") == 0) {
        cmd_import(jv_get_str(root, "path", ""), jv_get_int(root, "policy", 0), out);
    } else if (strcmp(cmd, "back") == 0) {
        aic_back_action act = aic_router_back(g_router);
        jw w;
        sb_reset(out);
        jw_init(&w, out, 0);
        jw_obj_begin(&w, NULL);
        jw_put_bool(&w, "ok", 1);
        jw_put_str(&w, "err", "");
        jw_put_int(&w, "action", (long long)act);
        jw_obj_end(&w);
        if (act == AIC_BACK_CLOSED_OVERLAY) g_conv[0] = 0;
    } else if (strcmp(cmd, "openOverlay") == 0) {
        rc = aic_router_open_overlay(g_router, (aic_overlay_kind)jv_get_int(root, "kind", 1));
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "closeOverlay") == 0) {
        rc = aic_router_close_overlay(g_router);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "deleteThread") == 0) {
        rc = aic_conv_delete(g_db, jv_get_str(root, "id", ""));
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc));
        else { if (strcmp(g_conv, jv_get_str(root, "id", "")) == 0) g_conv[0] = 0; reply_ok(out); }
    } else if (strcmp(cmd, "pinThread") == 0) {
        rc = aic_conv_set_pinned(g_db, jv_get_str(root, "id", ""), jv_get_bool(root, "pinned", 1));
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "balance") == 0) {
        aic_usage_summary sum;
        aic_llm_balance bal;
        char money[32], used[32];
        jw w;
        memset(&sum, 0, sizeof(sum));
        memset(&bal, 0, sizeof(bal));
        aic_usage_summary_get(g_db, 0, &sum);
        aic_llm_balance_keep_last(&bal, NULL, aic_now_ms());
        aic_llm_format_micros(sum.cost_micros, used, sizeof(used));
        bal.local_used_micros = sum.cost_micros;
        aic_llm_format_micros(bal.local_used_micros, money, sizeof(money));
        sb_reset(out);
        jw_init(&w, out, 0);
        jw_obj_begin(&w, NULL);
        jw_put_bool(&w, "ok", 1);
        jw_put_str(&w, "err", "");
        jw_put_bool(&w, "valid", 0);
        jw_put_bool(&w, "stale", 1);
        jw_put_str(&w, "available", "未查询");
        jw_put_str(&w, "localUsed", money);
        jw_put_str(&w, "note", "Android 端未接入账户余额接口，当前仅展示本地账本");
        jw_obj_end(&w);
    } else if (strcmp(cmd, "bench") == 0) {
        jw w;
        int i;
        sb_reset(out);
        jw_init(&w, out, 0);
        jw_obj_begin(&w, NULL);
        jw_put_bool(&w, "ok", 1);
        jw_put_str(&w, "err", "");
        jw_arr_begin(&w, "metrics");
        for (i = 0; i < AIC_PERF_METRIC_COUNT; i++) {
            jw_obj_begin(&w, NULL);
            jw_put_str(&w, "metric", aic_perf_metric_name((aic_perf_metric)i));
            jw_put_int(&w, "budget",
                       (long long)aic_perf_budget_value(AIC_PLATFORM_MOBILE, (aic_perf_metric)i));
            jw_put_str(&w, "unit", aic_perf_metric_unit((aic_perf_metric)i));
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    } else {
        reply_err(out, "未知命令");
    }
    jv_free(root);
}

JNIEXPORT jstring JNICALL
Java_com_aichat_app_Native_call(JNIEnv *env, jclass cls, jstring jreq)
{
    const char *req;
    sb out;
    jstring ret;

    (void)cls;
    if (!jreq) return NULL;
    req = (*env)->GetStringUTFChars(env, jreq, NULL);
    if (!req) return NULL;
    sb_init(&out);
    handle(req, strlen(req), &out);
    (*env)->ReleaseStringUTFChars(env, jreq, req);
    ret = (*env)->NewStringUTF(env, sb_cstr(&out));
    sb_free(&out);
    return ret;
}

JNIEXPORT void JNICALL
Java_com_aichat_app_Native_shutdown(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_eng) { aic_engine_destroy(g_eng); g_eng = NULL; }
    if (g_router) { aic_router_destroy(g_router); g_router = NULL; }
    if (g_db) { aic_db_close(g_db); g_db = NULL; }
}
