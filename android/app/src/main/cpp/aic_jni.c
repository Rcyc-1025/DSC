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
#include <pthread.h>
#include <unistd.h>
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
#include "zip.h"
#include "update.h"
#include "backup.h"
#include "avatar.h"

#define TAG "SaiCore"
#define JLOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* 消息默认只给最近 100 条，更早的由 loadOlder 每次向上翻 100 条（技术设计 13.3）。 */
#define AIC_MSG_PAGE 100

static aic_db     *g_db;
static aic_engine *g_eng;
static aic_router *g_router;
static char        g_dir[400];       /* 应用私有根目录 */
static char        g_conv[40];       /* 当前查看的会话 id（问答线程或唯一聊天会话） */
static char        g_err[512];

/* ============================================================================
 * 以下为 Android 端补齐部分（对应技术设计文档 FR-02/03/05/07/08/11/16/17 等）：
 *   1) 真实网络：三段协议（C 侧组装请求 + 喂 SSE 解析器，Java 侧做 HTTP）
 *   2) 停止生成、重新生成
 *   3) 头像路径、跟随系统深色
 *   4) 更新检测 / 校验 / 安装交接
 *   5) 备份、导出、导入
 *   6) 插件清单管理
 * 全部仍然只通过 call(json) 一个入口暴露，界面层不直接碰核心数据结构。
 * ========================================================================== */

/* 后面定义的函数先声明（本文件实现顺序：工具 → 命令 → 分发）。 */
static int ensure_env(void);
static const char *current_conv(void);
static void reply_ok(sb *out);
static void reply_err(sb *out, const char *msg);

/* 进程内的可变状态：全部只保存在内存里，不落盘。 */
static char g_key[256];      /* API Key：不进日志、不进导出、不进备份包 */
static int  g_system_night;  /* 系统是否深色（Java 侧读 uiMode 后传进来） */
static int  g_backup_ok;     /* 本进程内是否成功备份过（更新安装的前置条件） */

/* 最近一次解析成功的更新清单，下载校验与安装校验都要用它。 */
static aic_update_manifest g_update;
static int                 g_update_valid;

/* ---------- 三段协议：一次生成的状态 ----------
 * C 侧只负责「组装请求体」与「把 Java 收到的每一块字节喂给 SSE 解析器」，
 * 真正的 HTTP 由 Java 的 HttpURLConnection 完成（Android 上没有 WinHTTP）。 */
typedef struct aic_gen {
    int      active;        /* 1 表示有进行中的生成 */
    int      handle;        /* 本次生成句柄（Java 侧回传，用于防错配） */
    int      regen;         /* 1 表示「重新生成」 */
    char     conv_id[64];
    char     model[64];
    aic_sse_parser parser;
    int      parser_inited;
    sb       text;          /* 已收到的全部增量文本 */
    sb       body;          /* 请求体（服务端不给 usage 时用它估算 prompt token） */
    size_t   emitted;       /* 已经回给 Java 的字节数，只需回新增部分 */
    long long pt, ct;       /* 用量 */
    aic_llm_err err;        /* 解析过程中服务端返回的错误 */
    char     err_msg[256];
    int      cancel;        /* 用户点了停止 */
} aic_gen;

static aic_gen      g_gen;
static int          g_gen_ready;  /* text/body 两个 sb 是否已初始化 */
static int          g_gen_seq;    /* 句柄自增 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void gen_lock(void)   { pthread_mutex_lock(&g_lock); }
static void gen_unlock(void) { pthread_mutex_unlock(&g_lock); }

/* 关闭整个环境（恢复备份前必须调用：恢复会替换数据库文件）。 */
static void close_env(void)
{
    if (g_eng) { aic_engine_destroy(g_eng); g_eng = NULL; }
    if (g_router) { aic_router_destroy(g_router); g_router = NULL; }
    if (g_db) { aic_db_close(g_db); g_db = NULL; }
}

/* 当前生效模型：与 aic_engine_build_request 的取值顺序一致（设置优先）。 */
static const char *selected_model(void)
{
    if (aic_setting_get_int(g_db, AIC_SET_MODEL_DEFAULT, 0) == 1) return AIC_MODEL_PRO;
    return AIC_MODEL_FLASH;
}

/* 服务端地址：设置里有就用设置的，否则用默认端点。 */
static const char *cur_endpoint(void)
{
    static char ep[AIC_LLM_URL_MAX];
    ep[0] = '\0';
    aic_setting_get(g_db, AIC_SET_CRED_ENDPOINT, ep, sizeof(ep));
    if (!ep[0]) aic_strlcpy_utf8(ep, sizeof(ep), AIC_LLM_MODEL_DEFAULT_URL);
    return ep;
}

static void gen_reset(void)
{
    if (!g_gen_ready) {
        sb_init(&g_gen.text);
        sb_init(&g_gen.body);
        g_gen_ready = 1;
    }
    if (g_gen.parser_inited) {
        aic_sse_free(&g_gen.parser);
        g_gen.parser_inited = 0;
    }
    memset(&g_gen.parser, 0, sizeof(g_gen.parser));
    sb_reset(&g_gen.text);
    sb_reset(&g_gen.body);
    g_gen.active = 0;
    g_gen.handle = 0;
    g_gen.regen = 0;
    g_gen.conv_id[0] = '\0';
    g_gen.model[0] = '\0';
    g_gen.emitted = 0;
    g_gen.pt = g_gen.ct = 0;
    g_gen.err = AIC_LLM_OK;
    g_gen.err_msg[0] = '\0';
    g_gen.cancel = 0;
}

/* SSE 事件回调：只累加文本与用量，不碰界面（界面靠 feed 返回的增量刷新）。 */
static void gen_on_event(void *ud, const aic_llm_event *ev)
{
    aic_gen *g = (aic_gen *)ud;
    if (!g || !ev) return;
    if (ev->kind == AIC_LLM_EV_DELTA && ev->text) {
        sb_append(&g->text, ev->text);
    } else if (ev->kind == AIC_LLM_EV_USAGE) {
        if (ev->prompt_tokens > 0) g->pt = ev->prompt_tokens;
        if (ev->completion_tokens > 0) g->ct = ev->completion_tokens;
    } else if (ev->kind == AIC_LLM_EV_ERROR) {
        g->err = ev->err;
        aic_strlcpy_utf8(g->err_msg, sizeof(g->err_msg), ev->message);
    }
}

/* 粗略 token 估算：流式响应通常不返回 usage，只能按字符数估（中文按字节数补偿）。 */
static long long est_tokens(const char *s)
{
    size_t i, n, wide = 0;
    if (!s) return 0;
    n = strlen(s);
    for (i = 0; i < n; i++) {
        if ((unsigned char)s[i] >= 0x80) wide++;
    }
    return (long long)(n / 4 + wide / 4 + 1);
}

static void reply_ok_note(sb *out, const char *note)
{
    jw w;
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "note", note ? note : "");
    jw_obj_end(&w);
}

static void reply_err_code(sb *out, const char *msg, int code)
{
    jw w;
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 0);
    jw_put_str(&w, "err", msg ? msg : "未知错误");
    jw_put_int(&w, "errCode", (long long)code);
    jw_obj_end(&w);
}

/* 递归删除目录（插件导入后的临时目录清理）。 */
static void rm_tree(const char *dir)
{
    sb names;
    int n;
    sb_init(&names);
    n = aic_dir_list(dir, &names, '\n');
    if (n > 0) {
        const char *p = sb_cstr(&names);
        while (*p) {
            char name[256];
            char child[700];
            size_t k = 0;
            while (*p && *p != '\n' && k + 1 < sizeof(name)) name[k++] = *p++;
            name[k] = '\0';
            if (*p == '\n') p++;
            if (!name[0]) continue;
            aic_path_join(child, sizeof(child), dir, name);
            if (aic_is_dir(child)) rm_tree(child);
            else aic_file_remove(child);
        }
    }
    sb_free(&names);
    remove(dir);
}

/* ---------- 三段协议命令 ---------- */

/* sendBegin：落用户消息 → 组装请求体 → 把 url/apiKey/body 交给 Java 去发。 */
static void cmd_send_begin(const char *text, int regen, sb *out)
{
    char err[256] = { 0 };
    const char *conv;
    char conv_id[64];
    aic_message m;
    jw w;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!g_key[0]) {
        /* 没有 Key 就明确不联网：与离线演示传输保持同一套提示与错误码。 */
        reply_err_code(out, "尚未配置 API Key，请先到设置里填写", (int)AIC_LLM_E_AUTH);
        return;
    }
    conv = current_conv();
    if (!conv[0]) { reply_err(out, "没有可用的会话"); return; }

    gen_lock();
    if (g_gen.active) {
        gen_unlock();
        reply_err(out, "上一次回复还没有结束，请先停止");
        return;
    }
    gen_reset();
    aic_strlcpy_utf8(conv_id, sizeof(conv_id), conv);
    if (regen) {
        memset(&m, 0, sizeof(m));
        if (aic_msg_last_assistant(g_db, conv_id, &m) != AIC_OK) {
            gen_unlock();
            reply_err(out, "还没有可以重新生成的回复");
            return;
        }
        aic_msg_supersede(g_db, m.id);   /* 旧回复标记为被替代，不删除 */
        aic_msg_free(&m);
    } else {
        if (!text || !text[0]) { gen_unlock(); reply_err(out, "消息不能为空"); return; }
        memset(&m, 0, sizeof(m));
        rc = aic_msg_append(g_db, conv_id, AIC_ROLE_USER, text, selected_model(), &m);
        if (rc != AIC_OK) { gen_unlock(); reply_err(out, "保存消息失败"); return; }
        aic_msg_free(&m);
    }
    rc = aic_engine_build_request(g_eng, conv_id, &g_gen.body, err, sizeof(err));
    if (rc != AIC_OK) {
        gen_unlock();
        reply_err(out, err[0] ? err : "组装请求失败");
        return;
    }
    aic_sse_init(&g_gen.parser);
    g_gen.parser_inited = 1;
    g_gen.active = 1;
    g_gen.handle = ++g_gen_seq;
    g_gen.regen = regen ? 1 : 0;
    aic_strlcpy_utf8(g_gen.conv_id, sizeof(g_gen.conv_id), conv_id);
    aic_strlcpy_utf8(g_gen.model, sizeof(g_gen.model), selected_model());

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_int(&w, "handle", (long long)g_gen.handle);
    jw_put_str(&w, "url", cur_endpoint());
    jw_put_str(&w, "apiKey", g_key);
    jw_put_str(&w, "body", sb_cstr(&g_gen.body));
    jw_put_str(&w, "convId", conv_id);
    jw_put_str(&w, "model", g_gen.model);
    jw_obj_end(&w);
    gen_unlock();
}

/* feed：Java 每读到一块字节就调一次，这里喂给 SSE 解析器并只回新增文本。 */
static void gen_feed_data(const void *data, size_t len, sb *out)
{
    jw w;
    sb delta;
    const char *all;
    size_t from, to;

    gen_lock();
    if (!g_gen.active) {
        gen_unlock();
        reply_err(out, "没有正在进行的回复");
        return;
    }
    if (g_gen.cancel) {
        sb_reset(out);
        jw_init(&w, out, 0);
        jw_obj_begin(&w, NULL);
        jw_put_bool(&w, "ok", 0);
        jw_put_str(&w, "err", "已停止生成");
        jw_put_bool(&w, "cancelled", 1);
        jw_obj_end(&w);
        gen_unlock();
        return;
    }
    if (len > 0) aic_sse_feed(&g_gen.parser, (const char *)data, len, gen_on_event, &g_gen);

    all = sb_cstr(&g_gen.text);
    to = g_gen.text.len;
    from = g_gen.emitted;
    if (from > to) from = to;
    sb_init(&delta);
    if (to > from) sb_appendn(&delta, all + from, to - from);
    g_gen.emitted = to;

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "delta", sb_cstr(&delta));
    jw_put_bool(&w, "done", g_gen.parser.finished);
    jw_put_int(&w, "errCode", (long long)g_gen.err);
    jw_put_str(&w, "errMsg", g_gen.err_msg);
    jw_obj_end(&w);
    sb_free(&delta);
    gen_unlock();
}

/* sendEnd：收尾 → 成功则落助手消息并记账；用户停止则只保留用户消息。 */
static void cmd_send_end(int handle, sb *out)
{
    aic_message amsg;
    const aic_llm_model *mdl;
    long long pt, ct, cost;
    char cost_s[32] = { 0 };
    jw w;

    gen_lock();
    if (!g_gen.active || (handle > 0 && handle != g_gen.handle)) {
        gen_unlock();
        reply_err(out, "没有匹配的回复任务");
        return;
    }
    if (g_gen.cancel) {
        sb_reset(out);
        jw_init(&w, out, 0);
        jw_obj_begin(&w, NULL);
        jw_put_bool(&w, "ok", 0);
        jw_put_str(&w, "err", "已停止生成");
        jw_put_int(&w, "errCode", (long long)AIC_LLM_E_CANCEL);
        jw_put_bool(&w, "cancelled", 1);
        jw_put_str(&w, "text", sb_cstr(&g_gen.text));
        jw_obj_end(&w);
        gen_reset();
        gen_unlock();
        return;
    }
    if (g_gen.parser_inited) aic_sse_finish(&g_gen.parser, gen_on_event, &g_gen);
    if (g_gen.err != AIC_LLM_OK) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s",
                 g_gen.err_msg[0] ? g_gen.err_msg : aic_llm_err_text(g_gen.err));
        reply_err_code(out, msg, (int)g_gen.err);
        gen_reset();
        gen_unlock();
        return;
    }
    if (g_gen.text.len == 0) {
        reply_err(out, "模型没有返回内容");
        gen_reset();
        gen_unlock();
        return;
    }
    memset(&amsg, 0, sizeof(amsg));
    if (aic_msg_append(g_db, g_gen.conv_id, AIC_ROLE_ASSISTANT, sb_cstr(&g_gen.text),
                       g_gen.model, &amsg) != AIC_OK) {
        reply_err(out, "保存回复失败");
        gen_reset();
        gen_unlock();
        return;
    }
    pt = g_gen.pt;
    ct = g_gen.ct;
    if (pt <= 0) pt = est_tokens(sb_cstr(&g_gen.body));
    if (ct <= 0) ct = est_tokens(sb_cstr(&g_gen.text));
    mdl = aic_llm_model_or_default(g_gen.model);
    cost = mdl ? aic_llm_cost_micros(mdl, pt, ct) : 0;
    aic_usage_append(g_db, g_gen.conv_id, amsg.id, mdl ? mdl->id : g_gen.model, pt, ct, cost);
    aic_llm_format_micros(cost, cost_s, sizeof(cost_s));

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "text", sb_cstr(&g_gen.text));
    jw_put_str(&w, "assistantId", amsg.id);
    jw_put_str(&w, "model", g_gen.model);
    jw_put_int(&w, "promptTokens", pt);
    jw_put_int(&w, "completionTokens", ct);
    jw_put_int(&w, "costMicros", cost);
    jw_put_str(&w, "cost", cost_s);
    jw_put_bool(&w, "regenerated", g_gen.regen);
    jw_obj_end(&w);
    aic_msg_free(&amsg);
    gen_reset();
    gen_unlock();
}

/* sendFail：Java 侧 HTTP 失败（非 2xx / 连接异常）时交回 C 侧，
 * 由核心按状态码分类成 AIC_LLM_E_*，界面据此提示可执行的下一步。 */
static void cmd_send_fail(int handle, int http, const char *message, sb *out)
{
    aic_llm_err le;
    char msg[420];

    gen_lock();
    if (!g_gen.active || (handle > 0 && handle != g_gen.handle)) {
        gen_unlock();
        reply_err(out, "没有匹配的回复任务");
        return;
    }
    if (http > 0) {
        le = aic_llm_classify_http(http);
        if (le == AIC_LLM_OK) le = AIC_LLM_E_SERVER;
    } else {
        le = AIC_LLM_E_NETWORK;
    }
    /* 服务端返回的 JSON 原样给用户看不懂，改用分类文案；纯文本错误则优先用原文。 */
    if (message && message[0] && message[0] != '{') snprintf(msg, sizeof(msg), "%s", message);
    else snprintf(msg, sizeof(msg), "%s", aic_llm_err_text(le));
    JLOG("生成失败：http=%d %s", http, msg);
    reply_err_code(out, msg, (int)le);
    gen_reset();
    gen_unlock();
}

/* stop：置取消标志，Java 侧配合 disconnect 断开连接。 */
static void cmd_stop(sb *out)
{
    gen_lock();
    if (!g_gen.active) {
        gen_unlock();
        reply_err(out, "当前没有正在生成的回复");
        return;
    }
    g_gen.cancel = 1;
    if (g_eng) aic_engine_stop(g_eng);   /* 旧的同步 send 路径也看这个标志 */
    gen_unlock();
    reply_ok(out);
}

/* ---------- 头像 ---------- */
static void cmd_set_avatar(const char *rel, sb *out)
{
    char err[256] = { 0 };
    char full[700];
    aic_profile p;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!aic_is_local_relative_path(rel)) {
        reply_err(out, "头像路径必须是应用私有目录下的相对路径");
        return;
    }
    aic_path_join(full, sizeof(full), g_dir, rel);
    if (!aic_path_exists(full)) { reply_err(out, "头像文件不存在"); return; }
    memset(&p, 0, sizeof(p));
    aic_profile_get(g_db, &p);
    aic_strlcpy_utf8(p.avatar_path, sizeof(p.avatar_path), rel);
    rc = aic_profile_set(g_db, &p, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "保存头像路径失败"); return; }
    reply_ok(out);
}

/* ---------- 更新 ---------- */
static const char *update_status_name(aic_update_status s)
{
    switch (s) {
    case AIC_UPD_UP_TO_DATE: return "up_to_date";
    case AIC_UPD_AVAILABLE:  return "available";
    case AIC_UPD_SKIPPED:    return "skipped";
    case AIC_UPD_TOO_OLD:    return "too_old";
    case AIC_UPD_INVALID:    return "invalid";
    default:                 return "not_checked";
    }
}

/* checkUpdate：清单 JSON 由 Java 用 HttpURLConnection 拉下来，判定逻辑仍在核心层。 */
static void cmd_check_update(const char *json, const char *cur, const char *skip, sb *out)
{
    char err[256] = { 0 };
    char now_s[32];
    aic_update_plan plan;
    jw w;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!json || !json[0]) { reply_err(out, "更新清单为空"); return; }
    memset(&g_update, 0, sizeof(g_update));
    rc = aic_update_parse_manifest(json, strlen(json), &g_update, err, sizeof(err));
    if (rc != AIC_OK) {
        g_update_valid = 0;
        reply_err(out, err[0] ? err : "更新清单格式不正确");
        return;
    }
    g_update_valid = 1;
    snprintf(now_s, sizeof(now_s), "%lld", (long long)aic_now_ms());
    aic_setting_set(g_db, "update.last_check_ms", now_s);
    memset(&plan, 0, sizeof(plan));
    aic_update_make_plan(&g_update, (cur && cur[0]) ? cur : "1.0.0", skip ? skip : "", &plan);

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "status", update_status_name(plan.status));
    jw_put_int(&w, "statusCode", (long long)plan.status);
    jw_put_str(&w, "currentVersion", plan.current_version);
    jw_put_str(&w, "targetVersion", plan.target_version);
    jw_put_str(&w, "channel", g_update.channel);
    jw_put_str(&w, "releasedAt", g_update.released_at);
    jw_put_str(&w, "notes", g_update.notes);
    jw_put_str(&w, "url", g_update.url);
    jw_put_str(&w, "sha256", g_update.sha256);
    jw_put_int(&w, "size", g_update.size);
    jw_put_bool(&w, "mandatory", plan.mandatory);
    jw_put_bool(&w, "needsFullPackage", plan.needs_full_package);
    jw_obj_end(&w);
}

/* downloadUpdate：Java 下载完成后调用，这里做大小 + SHA-256 校验。 */
static void cmd_download_update(const char *path, sb *out)
{
    char err[256] = { 0 };
    int64_t size = 0;
    jw w;
    int rc;

    if (!g_update_valid) { reply_err(out, "还没有可用的更新清单，请先检查更新"); return; }
    if (!path || !path[0] || !aic_path_exists(path)) { reply_err(out, "安装包文件不存在"); return; }
    rc = aic_update_verify_file(&g_update, path, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "安装包校验失败"); return; }
    aic_file_size(path, &size);

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "path", path);
    jw_put_int(&w, "size", (long long)size);
    jw_put_str(&w, "sha256", g_update.sha256);
    jw_put_str(&w, "version", g_update.version);
    jw_obj_end(&w);
}

/* installUpdate：核心只做「备份前置 + 校验」；真正拉起系统安装器是 Java 的事。 */
static void cmd_install_update(const char *path, int backup_ok, sb *out)
{
    char err[256] = { 0 };
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!g_update_valid) { reply_err(out, "还没有可用的更新清单，请先检查更新"); return; }
    if (!path || !path[0] || !aic_path_exists(path)) { reply_err(out, "安装包文件不存在"); return; }
    rc = aic_update_can_apply(backup_ok, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "需要先完成一次备份才能安装更新"); return; }
    rc = aic_update_verify_file(&g_update, path, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "安装包校验失败"); return; }
    reply_ok_note(out, "已通过大小与 SHA-256 校验，安装动作交给系统安装器");
}

/* ---------- 备份 ---------- */
static void cmd_backup_create(const char *ver, sb *out)
{
    char dir[512], dbpath[700], zip[700], name[128], err[256] = { 0 };
    aic_backup_info info;
    jw w;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    aic_path_join(dir, sizeof(dir), g_dir, "backups");
    aic_mkdir_p(dir);
    aic_path_join(dbpath, sizeof(dbpath), g_dir, "db/aic.db");
    snprintf(name, sizeof(name), "backup-%lld.zip", (long long)(aic_now_ms() / 1000));
    aic_path_join(zip, sizeof(zip), dir, name);
    memset(&info, 0, sizeof(info));
    rc = aic_backup_create(g_dir, dbpath, (ver && ver[0]) ? ver : "1.0.0", zip, &info,
                           err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "备份失败"); return; }
    g_backup_ok = 1;

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "path", zip);
    jw_put_str(&w, "name", name);
    jw_put_str(&w, "createdAt", info.created_at);
    jw_put_str(&w, "appVersion", info.app_version);
    jw_put_int(&w, "fileCount", info.file_count);
    jw_put_int(&w, "totalBytes", info.total_bytes);
    jw_obj_end(&w);
}

static void cmd_backup_list(sb *out)
{
    char dir[512], child[700], err[128] = { 0 };
    sb names;
    jw w;
    int n;
    const char *p;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    aic_path_join(dir, sizeof(dir), g_dir, "backups");
    sb_init(&names);
    n = aic_dir_list(dir, &names, '\n');
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "dir", dir);
    jw_arr_begin(&w, "items");
    if (n > 0) {
        p = sb_cstr(&names);
        while (*p) {
            char name[256];
            size_t k = 0;
            int64_t size = 0;
            aic_backup_info info;
            memset(&info, 0, sizeof(info));
            while (*p && *p != '\n' && k + 1 < sizeof(name)) name[k++] = *p++;
            name[k] = '\0';
            if (*p == '\n') p++;
            if (!name[0]) continue;
            aic_path_join(child, sizeof(child), dir, name);
            aic_file_size(child, &size);
            /* 清单读不出来的包照样列出，但 createdAt 为空——界面据此提示可能残缺 */
            aic_backup_inspect(child, &info, err, sizeof(err));
            jw_obj_begin(&w, NULL);
            jw_put_str(&w, "name", name);
            jw_put_str(&w, "path", child);
            jw_put_int(&w, "size", (long long)size);
            jw_put_str(&w, "createdAt", info.created_at);
            jw_put_str(&w, "appVersion", info.app_version);
            jw_put_bool(&w, "readable", info.created_at[0] ? 1 : 0);
            jw_obj_end(&w);
        }
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    sb_free(&names);
}

static void cmd_backup_restore(const char *zip, sb *out)
{
    char err[256] = { 0 };
    int rc;

    if (!g_dir[0]) { reply_err(out, "尚未调用 init"); return; }
    if (!zip || !zip[0] || !aic_path_exists(zip)) { reply_err(out, "备份包不存在"); return; }
    gen_lock();
    if (g_gen.active) {
        gen_unlock();
        reply_err(out, "正在生成回复，请先停止再恢复备份");
        return;
    }
    gen_reset();
    gen_unlock();
    close_env();   /* 恢复会替换数据库文件，必须先关库 */
    rc = aic_backup_restore(zip, g_dir, err, sizeof(err));
    if (ensure_env() != AIC_OK) { reply_err(out, "恢复后重新打开数据库失败"); return; }
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "恢复失败"); return; }
    g_conv[0] = 0;
    reply_ok(out);
}

static void cmd_backup_clean(int keep, sb *out)
{
    char dir[512], err[256] = { 0 };
    int removed = 0;
    jw w;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    aic_path_join(dir, sizeof(dir), g_dir, "backups");
    rc = aic_backup_clean(dir, keep < 1 ? 1 : keep, &removed, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "清理失败"); return; }
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_int(&w, "removed", removed);
    jw_obj_end(&w);
}

/* ---------- 导出 / 导入（路径由 Java 选好，位于应用私有目录） ---------- */
/* 导出范围（技术设计 8.1）：range=current|all|days7、withAvatars=0|1。
 * conv_buf 由调用方给：aic_export_scope 里存的是指针，不能指向栈上的临时串。 */
static int fill_export_scope(const jv *root, aic_export_scope *sc, char *conv_buf, size_t conv_cap)
{
    const char *range = jv_get_str(root, "range", "all");

    memset(sc, 0, sizeof(*sc));
    sc->include_avatars = jv_get_int(root, "withAvatars", 0) ? 1 : 0;
    sc->root = g_dir;
    if (strcmp(range, "current") == 0) {
        const char *c = jv_get_str(root, "conv", "");
        if (!c[0]) c = current_conv();
        if (!c[0]) return 0;
        aic_strlcpy_utf8(conv_buf, conv_cap, c);
        sc->scope = 1;
        sc->conv_id = conv_buf;
    } else if (strcmp(range, "days7") == 0) {
        sc->scope = 2;
        sc->to_ms = aic_now_ms();
        sc->from_ms = sc->to_ms - 7LL * 24LL * 3600LL * 1000LL;
    } else {
        sc->scope = 0;   /* all：按模式导出全部会话 */
    }
    return 1;
}

static const char *scope_tag(const aic_export_scope *sc)
{
    if (!sc) return "all";
    if (sc->scope == 1) return "current";
    if (sc->scope == 2) return "days7";
    return "all";
}

static void cmd_export_json(const char *path, const aic_export_scope *sc, sb *out)
{
    char err[256] = { 0 };
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!path || !path[0]) { reply_err(out, "导出路径为空"); return; }
    rc = aic_export_write_json_ex(g_db, -1, sc, path, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "导出失败"); return; }
    reply_ok_note(out, path);
}

static void cmd_import_json(const char *path, int policy, sb *out)
{
    char err[256] = { 0 };
    aic_import_preview pv;
    aic_import_result ir;
    jw w;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!path || !path[0] || !aic_path_exists(path)) { reply_err(out, "导入文件不存在"); return; }
    memset(&pv, 0, sizeof(pv));
    rc = aic_transfer_preview(g_db, path, &pv, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "导入文件无法识别"); return; }
    memset(&ir, 0, sizeof(ir));
    rc = aic_transfer_import(g_db, path, (aic_conflict_policy)policy, &ir, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "导入失败"); return; }

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_int(&w, "convAdded", ir.conv_added);
    jw_put_int(&w, "convRenamed", ir.conv_renamed);
    jw_put_int(&w, "convSkipped", ir.conv_skipped);
    jw_put_int(&w, "msgAdded", ir.msg_added);
    jw_put_int(&w, "msgRenamed", ir.msg_renamed);
    jw_put_int(&w, "msgSkipped", ir.msg_skipped);
    jw_put_int(&w, "previewConversations", pv.conversations);
    jw_put_int(&w, "previewMessages", pv.messages);
    jw_obj_end(&w);
}

/* ---------- 插件 ---------- */
static void cmd_plugin_list(sb *out)
{
    aic_plugin *arr = NULL;
    size_t n = 0, i;
    jw w;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (aic_plugin_list(g_db, -1, &arr, &n) != AIC_OK) { arr = NULL; n = 0; }
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_bool(&w, "procAvailable", 0);   /* Android 上插件进程隔离不可用 */
    jw_put_str(&w, "procNote", "Android 上仅可管理清单，扩展点运行需桌面端");
    jw_arr_begin(&w, "items");
    for (i = 0; i < n; i++) {
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "id", arr[i].id);
        jw_put_str(&w, "name", arr[i].name);
        jw_put_str(&w, "version", arr[i].version);
        jw_put_str(&w, "author", arr[i].author);
        jw_put_str(&w, "extensionPoints", arr[i].extension_points);
        jw_put_str(&w, "permissions", arr[i].permissions);
        jw_put_bool(&w, "enabled", arr[i].enabled);
        jw_put_str(&w, "installedAt", arr[i].installed_at);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    if (arr) aic_plugin_free_array(arr, n);
}

/* 插件 zip 先解到临时目录并定位清单（优先 plugin.json，兼容 manifest.json）；成功时 *work 是临时目录，
 * 调用方负责 rm_tree(*work)。pluginInspect 与 pluginInstall 共用这一步。 */
static int unpack_plugin_zip(const char *zip, char *work, size_t workcap,
                             char *manifest, size_t mancap, char *err, size_t errlen)
{
    char tmp[700], uuid[37], child[800], inner[900];
    sb names;
    int rc, n;

    if (err && errlen) err[0] = '\0';
    if (!zip || !zip[0] || !aic_path_exists(zip)) {
        if (err) snprintf(err, errlen, "插件包不存在");
        return AIC_E_INVAL;
    }
    aic_uuid4(uuid);
    aic_path_join(tmp, sizeof(tmp), g_dir, "tmp");
    aic_mkdir_p(tmp);
    aic_path_join(child, sizeof(child), tmp, uuid);
    aic_mkdir_p(child);
    aic_strlcpy_utf8(work, workcap, child);
    rc = aic_zip_extract(zip, child, err, errlen);
    if (rc != AIC_OK) {
        rm_tree(child);
        if (err && !err[0]) snprintf(err, errlen, "插件包解压失败");
        return rc;
    }
    /* 清单可能在包根，也可能在唯一的一层子目录里。 */
    aic_path_join(manifest, mancap, child, "plugin.json");
    if (!aic_path_exists(manifest)) {
        aic_path_join(manifest, mancap, child, "manifest.json");
        if (!aic_path_exists(manifest)) {
            sb_init(&names);
            n = aic_dir_list(child, &names, '\n');
            if (n > 0) {
                const char *p = sb_cstr(&names);
                char sub[256];
                size_t k = 0;
                while (*p && *p != '\n' && k + 1 < sizeof(sub)) sub[k++] = *p++;
                sub[k] = '\0';
                aic_path_join(inner, sizeof(inner), child, sub);
                aic_path_join(manifest, mancap, inner, "plugin.json");
                if (!aic_path_exists(manifest)) aic_path_join(manifest, mancap, inner, "manifest.json");
            }
            sb_free(&names);
            if (!aic_path_exists(manifest)) {
                rm_tree(child);
                if (err) snprintf(err, errlen, "插件包缺少 plugin.json");
                return AIC_E_INVAL;
            }
        }
    }
    return AIC_OK;
}

/* pluginInstall：用户已确认后才调用；overwrite=1 表示覆盖同名已安装插件（技术设计 10.2）。 */
static void cmd_plugin_install(const char *zip, int allow_overwrite, sb *out)
{
    char err[256] = { 0 };
    char work[900], manifest[1000], plugins[700];
    aic_plugin pl;
    jw w;
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (unpack_plugin_zip(zip, work, sizeof(work), manifest, sizeof(manifest), err, sizeof(err)) != AIC_OK) {
        reply_err(out, err[0] ? err : "插件包处理失败");
        return;
    }
    aic_path_join(plugins, sizeof(plugins), g_dir, "plugins");
    aic_mkdir_p(plugins);
    memset(&pl, 0, sizeof(pl));
    rc = aic_plugin_install_ex(g_db, plugins, manifest, "zip", allow_overwrite ? 1 : 0,
                               &pl, err, sizeof(err));
    rm_tree(work);
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "插件安装失败"); return; }

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "id", pl.id);
    jw_put_str(&w, "name", pl.name);
    jw_put_str(&w, "version", pl.version);
    jw_put_str(&w, "author", pl.author);
    jw_put_bool(&w, "enabled", pl.enabled);
    jw_put_bool(&w, "overwriteAllowed", allow_overwrite ? 1 : 0);
    jw_put_str(&w, "note", "Android 上仅可管理清单，扩展点运行需桌面端；安装后默认禁用");
    jw_obj_end(&w);
    aic_plugin_free(&pl);
}

/* 清单里的权限/扩展点是逗号分隔串，界面按条目展示，这里拆成 JSON 数组。 */
static void jw_put_csv_array(jw *w, const char *key, const char *csv)
{
    char item[128];
    const char *p = csv;

    jw_arr_begin(w, key);
    while (p && *p) {
        size_t k = 0;
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        while (*p && *p != ',' && k + 1 < sizeof(item)) item[k++] = *p++;
        while (k > 0 && (item[k - 1] == ' ' || item[k - 1] == '\t')) k--;
        item[k] = '\0';
        if (k > 0) jw_put_str(w, NULL, item);
        while (*p && *p != ',') p++;   /* 超长条目跳过余下部分，避免死循环 */
    }
    jw_arr_end(w);
}

/* pluginInspect：安装前把清单（名称/版本/作者/权限/扩展点）读出来给用户确认，不落库。 */
static void cmd_plugin_inspect(const char *zip, sb *out)
{
    char err[256] = { 0 };
    char work[900], manifest[1000];
    char *text = NULL;
    size_t len = 0;
    aic_plugin_manifest mf;
    aic_plugin exist;
    int rc, exists = 0;
    jw w;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (unpack_plugin_zip(zip, work, sizeof(work), manifest, sizeof(manifest), err, sizeof(err)) != AIC_OK) {
        reply_err(out, err[0] ? err : "插件包处理失败");
        return;
    }
    rc = aic_file_read_all(manifest, &text, &len);
    if (rc != AIC_OK || !text) {
        rm_tree(work);
        reply_err(out, "插件清单读取失败");
        return;
    }
    memset(&mf, 0, sizeof(mf));
    rc = aic_plugin_parse_manifest(text, len, &mf, err, sizeof(err));
    aic_xfree(text);
    rm_tree(work);
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "插件清单格式不合法"); return; }

    memset(&exist, 0, sizeof(exist));
    exists = aic_plugin_get(g_db, mf.id, &exist) == AIC_OK;
    if (exists) aic_plugin_free(&exist);

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "id", mf.id);
    jw_put_str(&w, "name", mf.name);
    jw_put_str(&w, "version", mf.version);
    jw_put_str(&w, "author", mf.author);
    jw_put_str(&w, "entry", mf.entry);
    jw_put_csv_array(&w, "permissions", mf.permissions);
    jw_put_csv_array(&w, "extensionPoints", mf.extension_points);
    jw_put_bool(&w, "hasSha256", mf.sha256[0] ? 1 : 0);
    jw_put_bool(&w, "exists", exists ? 1 : 0);
    jw_put_str(&w, "procNote", "Android 上仅可管理清单，扩展点运行需桌面端");
    jw_obj_end(&w);
}

static void cmd_plugin_set_enabled(const char *id, int enabled, sb *out)
{
    char err[256] = { 0 };
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    rc = aic_plugin_set_enabled(g_db, id, enabled ? 1 : 0, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "更新插件状态失败"); return; }
    reply_ok_note(out, "Android 端的启用状态只登记清单，不会真正加载插件代码");
}

static void cmd_plugin_uninstall(const char *id, int keep_data, sb *out)
{
    char err[256] = { 0 };
    int rc;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    rc = aic_plugin_uninstall(g_db, id, keep_data ? 1 : 0, err, sizeof(err));
    if (rc != AIC_OK) { reply_err(out, err[0] ? err : "卸载插件失败"); return; }
    reply_ok(out);
}


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

/* 签名要与 aic_llm_transport.post_stream 完全一致（最后两个前多一个 cancel 指针）：
 * 离线演示是瞬时完成的，用不到取消标志，但指针位必须对上。 */
static aic_llm_err demo_post(void *ud, const char *url, const char *api_key, const char *body,
                             aic_sse_parser *parser, aic_llm_event_fn fn, void *cb_ud,
                             volatile int *cancel, char *err, size_t errlen)
{
    char p1[400], p2[400], p3[400];
    char *parts[3];
    sb chunk;
    int i;

    (void)ud; (void)url; (void)cancel;
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
    /* 恢复备份后会重新建引擎，内存里的 Key 要重新灌回去。 */
    if (g_key[0]) aic_engine_set_api_key(g_eng, g_key);
    /* 设置里选的模型交给引擎作为 override，否则请求里会一直用会话默认模型。 */
    aic_engine_set_model_override(g_eng, selected_model());
    aic_engine_set_endpoint(g_eng, cur_endpoint());
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
    /* 第二个参数是系统当前是否深色：pref=system 时靠它决定实际令牌（FR-17）。 */
    const aic_theme *th = aic_theme_resolve(pref, g_system_night);
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

/* 一串消息写进 key 数组：界面只读这些字段，不需要碰核心结构。 */
static void put_msg_items(jw *w, const char *key, aic_message *arr, size_t n)
{
    size_t i;

    jw_arr_begin(w, key);
    for (i = 0; i < n; i++) {
        jw_obj_begin(w, NULL);
        jw_put_str(w, "id", arr[i].id);
        jw_put_int(w, "seq", arr[i].seq);
        jw_put_str(w, "role", arr[i].role);
        jw_put_str(w, "content", arr[i].content ? arr[i].content : "");
        jw_put_bool(w, "recalled", arr[i].recalled);
        jw_put_bool(w, "superseded", arr[i].superseded);
        jw_put_bool(w, "edited", arr[i].edited_at[0] != 0);
        /* 有原文才能「撤销修改」（FR-07）：菜单只在这种消息上给入口。 */
        jw_put_bool(w, "hasOriginal", arr[i].original_content != NULL && arr[i].original_content[0] != 0);
        jw_put_str(w, "ts", arr[i].created_at);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* oldest 之前还有没有消息：拿 1 条探一下（比 COUNT 更省，且不依赖额外接口）。 */
static int has_older_msgs(const char *conv_id, long long oldest)
{
    aic_message *arr = NULL;
    size_t n = 0;
    int more = 0;

    if (!conv_id || !conv_id[0] || oldest <= 0) return 0;
    if (aic_msg_page(g_db, conv_id, oldest, 1, &arr, &n) == AIC_OK) {
        more = n > 0;
        aic_msg_free_array(arr, n);
    }
    return more;
}

/* 默认只给最近 AIC_MSG_PAGE 条 + hasMore/oldestSeq，向上翻页走 loadOlder。 */
static void put_msgs(jw *w, const char *conv_id)
{
    aic_message *arr = NULL;
    size_t n = 0;
    long long oldest = 0;

    if (conv_id && conv_id[0] && aic_msg_page(g_db, conv_id, 0, AIC_MSG_PAGE, &arr, &n) != AIC_OK) {
        arr = NULL;
        n = 0;
    }
    put_msg_items(w, "msgs", arr, n);
    if (n > 0) oldest = arr[0].seq;
    jw_put_int(w, "oldestSeq", oldest);
    jw_put_bool(w, "hasMore", has_older_msgs(conv_id, oldest) ? 1 : 0);
    jw_put_int(w, "pageSize", AIC_MSG_PAGE);
    if (arr) aic_msg_free_array(arr, n);
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
    jw_put_bool(&w, "hasKey", (aic_engine_has_key(g_eng) || g_key[0]) ? 1 : 0);
    jw_put_str(&w, "model", aic_setting_get_int(g_db, AIC_SET_MODEL_DEFAULT, 0) == 1
                                   ? AIC_MODEL_PRO : AIC_MODEL_FLASH);
    /* 界面要用的运行时事实：系统深色、是否正在生成、插件能否运行、更新与备份状态。 */
    jw_put_bool(&w, "systemNight", g_system_night ? 1 : 0);
    jw_put_bool(&w, "streaming", g_gen.active ? 1 : 0);
    jw_put_bool(&w, "pluginProcAvailable", 0);
    jw_put_str(&w, "pluginProcNote", "Android 上仅可管理清单，扩展点运行需桌面端");
    jw_put_bool(&w, "hasBackup", g_backup_ok ? 1 : 0);
    jw_put_int(&w, "balanceAlertCents", aic_setting_get_int(g_db, AIC_SET_BALANCE_ALERT, 0));
    jw_put_int(&w, "updateAuto", aic_setting_get_int(g_db, AIC_SET_UPDATE_AUTO, 1));
    jw_put_int(&w, "updateIntervalHours", aic_setting_get_int(g_db, AIC_SET_UPDATE_INTERVAL, 24));
    {
        char murl[512] = { 0 };
        char lastc[32] = { 0 };
        aic_setting_get(g_db, "update.manifest_url", murl, sizeof(murl));
        aic_setting_get(g_db, "update.last_check_ms", lastc, sizeof(lastc));
        jw_put_str(&w, "updateManifestUrl", murl);
        jw_put_int(&w, "lastUpdateCheckMs", (long long)atoll(lastc));
    }
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

static int cmd_export(const char *kind, const aic_export_scope *sc, sb *out)
{
    char dir[512], path[512], name[160], err[256] = { 0 };
    int rc;

    aic_path_join(dir, sizeof(dir), g_dir, "exports");
    aic_mkdir_p(dir);
    snprintf(name, sizeof(name), "chat_record_%lld_%s.%s",
             (long long)(aic_now_ms() / 1000), scope_tag(sc), strcmp(kind, "md") == 0 ? "md"
                     : (strcmp(kind, "txt") == 0 ? "txt" : "json"));
    aic_path_join(path, sizeof(path), dir, name);
    if (strcmp(kind, "md") == 0) rc = aic_export_write_markdown_ex(g_db, -1, sc, path, err, sizeof(err));
    else if (strcmp(kind, "txt") == 0) rc = aic_export_write_text_ex(g_db, -1, sc, path, err, sizeof(err));
    else rc = aic_export_write_json_ex(g_db, -1, sc, path, err, sizeof(err));
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

/* ---------- 消息分页：向上翻页（技术设计 13.3） ---------- */
static void cmd_load_older(const char *conv_in, long long before_seq, sb *out)
{
    const char *conv = (conv_in && conv_in[0]) ? conv_in : current_conv();
    aic_message *arr = NULL;
    size_t n = 0;
    long long oldest = 0;
    jw w;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!conv || !conv[0]) { reply_err(out, "没有可用的会话"); return; }
    if (before_seq <= 0) { reply_err(out, "缺少翻页起点（oldestSeq）"); return; }
    if (aic_msg_page(g_db, conv, before_seq, AIC_MSG_PAGE, &arr, &n) != AIC_OK) {
        reply_err(out, "读取更早消息失败");
        return;
    }
    if (n > 0) oldest = arr[0].seq;
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "conv", conv);
    put_msg_items(&w, "msgs", arr, n);
    jw_put_int(&w, "added", (long long)n);
    jw_put_int(&w, "oldestSeq", oldest);
    jw_put_bool(&w, "hasMore", has_older_msgs(conv, oldest) ? 1 : 0);
    jw_obj_end(&w);
    if (arr) aic_msg_free_array(arr, n);
}

/* ---------- 人设版本（FR-04）：列历史版本 + 用旧版本内容生成新版本 ---------- */
static void cmd_persona_versions(sb *out)
{
    aic_persona per;
    aic_prompt_version *arr = NULL;
    size_t n = 0, i;
    jw w;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    memset(&per, 0, sizeof(per));
    if (aic_persona_ensure_default(g_db, &per) != AIC_OK) {
        aic_persona_free(&per);
        reply_err(out, "人设读取失败");
        return;
    }
    aic_persona_versions(g_db, per.id, &arr, &n);
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "personaId", per.id);
    jw_put_int(&w, "current", per.current_version);
    jw_arr_begin(&w, "versions");
    /* 最近的版本排在前面，界面第一行就是当前版本。 */
    for (i = 0; i < n; i++) {
        size_t k = n - 1 - i;
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "id", arr[k].id);
        jw_put_int(&w, "version", arr[k].version);
        jw_put_str(&w, "createdAt", arr[k].created_at);
        jw_put_str(&w, "content", arr[k].content ? arr[k].content : "");
        jw_put_bool(&w, "current", arr[k].version == per.current_version ? 1 : 0);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    if (arr) aic_prompt_version_free_array(arr, n);
    aic_persona_free(&per);
}

/* 回退：把指定历史版本的内容当作一次「新的修改」写回去，历史不丢（版本号继续加）。 */
static void cmd_persona_rollback(const char *version_id, int version, sb *out)
{
    aic_persona per, upd;
    aic_prompt_version *arr = NULL;
    size_t n = 0, i;
    const char *pick = NULL;
    int rc;
    jw w;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    memset(&per, 0, sizeof(per));
    if (aic_persona_ensure_default(g_db, &per) != AIC_OK) {
        aic_persona_free(&per);
        reply_err(out, "人设读取失败");
        return;
    }
    aic_persona_versions(g_db, per.id, &arr, &n);
    for (i = 0; i < n; i++) {
        if (version_id && version_id[0]) {
            if (strcmp(arr[i].id, version_id) == 0) { pick = arr[i].content; break; }
        } else if (version > 0 && arr[i].version == version) {
            pick = arr[i].content;
            break;
        }
    }
    if (!pick) {
        if (arr) aic_prompt_version_free_array(arr, n);
        aic_persona_free(&per);
        reply_err(out, "指定的历史版本不存在");
        return;
    }
    memset(&upd, 0, sizeof(upd));
    rc = aic_persona_update(g_db, per.id, per.name, per.description, pick, &upd);
    if (arr) aic_prompt_version_free_array(arr, n);
    aic_persona_free(&per);
    if (rc != AIC_OK) { reply_err(out, aic_strerror(rc)); return; }

    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_int(&w, "version", upd.current_version);
    jw_obj_end(&w);
    aic_persona_free(&upd);
}

/* 问答会话重命名（FR-20 的历史会话管理）。 */
static void cmd_rename_thread(const char *id_in, const char *title, sb *out)
{
    const char *id = (id_in && id_in[0]) ? id_in : current_conv();
    int rc;
    jw w;

    if (ensure_env() != AIC_OK) { reply_err(out, g_err[0] ? g_err : "初始化失败"); return; }
    if (!id || !id[0]) { reply_err(out, "没有可用的会话"); return; }
    if (!title || !title[0]) { reply_err(out, "标题不能为空"); return; }
    rc = aic_conv_update_meta(g_db, id, title, NULL, NULL);
    if (rc != AIC_OK) { reply_err(out, aic_strerror(rc)); return; }
    sb_reset(out);
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_bool(&w, "ok", 1);
    jw_put_str(&w, "err", "");
    jw_put_str(&w, "id", id);
    jw_put_str(&w, "title", title);
    jw_obj_end(&w);
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
        else {
            /* models.json（技术设计 9.3）：存在就加载，失败只记日志，不影响启动。 */
            char mpath[600];
            aic_path_join(mpath, sizeof(mpath), g_dir, "models.json");
            if (aic_path_exists(mpath)) {
                char merr[256] = { 0 };
                if (aic_llm_models_load_file(mpath, merr, sizeof(merr)) != AIC_OK) {
                    JLOG("models.json 加载失败，已忽略：%s", merr[0] ? merr : "未知原因");
                }
            }
            reply_ok(out);
        }
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
        aic_strlcpy_utf8(g_key, sizeof(g_key), key);   /* 三段协议要用它拼 Authorization */
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
        aic_export_scope sc;
        char conv_buf[64] = { 0 };
        if (ensure_env() != AIC_OK) reply_err(out, g_err[0] ? g_err : "初始化失败");
        else if (!fill_export_scope(root, &sc, conv_buf, sizeof(conv_buf)))
            reply_err(out, "没有可导出的当前会话");
        else cmd_export(jv_get_str(root, "kind", "json"), &sc, out);
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
    } else if (strcmp(cmd, "setSystemNight") == 0) {
        /* Java 侧读 uiMode 后同步过来：ui.theme=system 时用它决定实际配色。 */
        g_system_night = jv_get_bool(root, "night", 0) ? 1 : 0;
        reply_ok(out);
    } else if (strcmp(cmd, "sendBegin") == 0) {
        cmd_send_begin(jv_get_str(root, "text", ""), jv_get_bool(root, "regen", 0), out);
    } else if (strcmp(cmd, "regenerate") == 0) {
        cmd_send_begin("", 1, out);
    } else if (strcmp(cmd, "sendEnd") == 0) {
        cmd_send_end((int)jv_get_int(root, "handle", 0), out);
    } else if (strcmp(cmd, "sendFail") == 0) {
        cmd_send_fail((int)jv_get_int(root, "handle", 0), (int)jv_get_int(root, "httpCode", 0),
                      jv_get_str(root, "message", ""), out);
    } else if (strcmp(cmd, "stop") == 0) {
        cmd_stop(out);
    } else if (strcmp(cmd, "setAvatarPath") == 0) {
        cmd_set_avatar(jv_get_str(root, "path", ""), out);
    } else if (strcmp(cmd, "checkUpdate") == 0) {
        cmd_check_update(jv_get_str(root, "json", ""), jv_get_str(root, "currentVersion", "1.0.0"),
                         jv_get_str(root, "skipVersion", ""), out);
    } else if (strcmp(cmd, "downloadUpdate") == 0) {
        cmd_download_update(jv_get_str(root, "path", ""), out);
    } else if (strcmp(cmd, "installUpdate") == 0) {
        cmd_install_update(jv_get_str(root, "path", ""), jv_get_bool(root, "backupOk", 0), out);
    } else if (strcmp(cmd, "setUpdateManifestUrl") == 0) {
        const char *url = jv_get_str(root, "url", "");
        rc = aic_setting_set(g_db, "update.manifest_url", url);
        if (rc != AIC_OK) reply_err(out, aic_strerror(rc)); else reply_ok(out);
    } else if (strcmp(cmd, "backupCreate") == 0) {
        cmd_backup_create(jv_get_str(root, "version", "1.0.0"), out);
    } else if (strcmp(cmd, "backupList") == 0) {
        cmd_backup_list(out);
    } else if (strcmp(cmd, "backupRestore") == 0) {
        cmd_backup_restore(jv_get_str(root, "zip", ""), out);
    } else if (strcmp(cmd, "backupClean") == 0) {
        cmd_backup_clean((int)jv_get_int(root, "keep", 5), out);
    } else if (strcmp(cmd, "exportJson") == 0) {
        aic_export_scope sc;
        char conv_buf[64] = { 0 };
        if (!fill_export_scope(root, &sc, conv_buf, sizeof(conv_buf)))
            reply_err(out, "没有可导出的当前会话");
        else cmd_export_json(jv_get_str(root, "path", ""), &sc, out);
    } else if (strcmp(cmd, "importJson") == 0) {
        cmd_import_json(jv_get_str(root, "path", ""), (int)jv_get_int(root, "policy", 0), out);
    } else if (strcmp(cmd, "pluginList") == 0) {
        cmd_plugin_list(out);
    } else if (strcmp(cmd, "pluginInspect") == 0) {
        cmd_plugin_inspect(jv_get_str(root, "path", ""), out);
    } else if (strcmp(cmd, "pluginInstall") == 0) {
        cmd_plugin_install(jv_get_str(root, "path", ""), jv_get_int(root, "overwrite", 0) ? 1 : 0, out);
    } else if (strcmp(cmd, "pluginSetEnabled") == 0) {
        cmd_plugin_set_enabled(jv_get_str(root, "id", ""), jv_get_bool(root, "enabled", 0), out);
    } else if (strcmp(cmd, "pluginUninstall") == 0) {
        cmd_plugin_uninstall(jv_get_str(root, "id", ""), jv_get_bool(root, "keepData", 0), out);
    } else if (strcmp(cmd, "loadOlder") == 0) {
        cmd_load_older(jv_get_str(root, "conv", ""), jv_get_int(root, "beforeSeq", 0), out);
    } else if (strcmp(cmd, "restoreEdit") == 0) {
        /* 撤销修改（FR-07）：content 回滚 original_content，清空 edited_at / original_content。 */
        rc = aic_msg_restore(g_db, jv_get_str(root, "id", ""));
        if (rc != AIC_OK) reply_err(out, "这条消息没有可撤销的修改");
        else reply_ok(out);
    } else if (strcmp(cmd, "personaVersions") == 0) {
        cmd_persona_versions(out);
    } else if (strcmp(cmd, "personaRollback") == 0) {
        cmd_persona_rollback(jv_get_str(root, "versionId", ""), (int)jv_get_int(root, "version", 0), out);
    } else if (strcmp(cmd, "renameThread") == 0) {
        cmd_rename_thread(jv_get_str(root, "id", ""), jv_get_str(root, "title", ""), out);
    } else {
        reply_err(out, "未知命令");
    }
    jv_free(root);
}

/* ---------- JNI 字符串转换 ----------
 * 不用 GetStringUTFChars / NewStringUTF：它们对 4 字节 UTF-8（emoji）不可靠。
 * 这里统一走 UTF-16 中转，C 侧内部仍是 UTF-8。
 * 补充：NewStringUTF 遇到 4 字节序列会被当非法修改字符，所以必须换掉。 */
static char *jstr_to_utf8(JNIEnv *env, jstring js)
{
    const jchar *u;
    jsize n, i;
    size_t cap, k = 0;
    char *out;

    u = (*env)->GetStringChars(env, js, NULL);
    if (!u) return NULL;
    n = (*env)->GetStringLength(env, js);
    cap = (size_t)n * 3 + 1;
    out = (char *)malloc(cap);
    if (!out) { (*env)->ReleaseStringChars(env, js, u); return NULL; }
    for (i = 0; i < n; i++) {
        unsigned int c = (unsigned int)u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            unsigned int lo = (unsigned int)u[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;
            }
        }
        if (c < 0x80u) {
            out[k++] = (char)c;
        } else if (c < 0x800u) {
            out[k++] = (char)(0xC0u | (c >> 6));
            out[k++] = (char)(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            out[k++] = (char)(0xE0u | (c >> 12));
            out[k++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[k++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            out[k++] = (char)(0xF0u | (c >> 18));
            out[k++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
            out[k++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[k++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    out[k] = '\0';
    (*env)->ReleaseStringChars(env, js, u);
    return out;
}

static jstring utf8_to_jstr(JNIEnv *env, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t cap = strlen((const char *)p) + 1, n = 0;
    jchar *buf;
    jstring ret;

    buf = (jchar *)malloc(cap * sizeof(jchar));
    if (!buf) return NULL;
    while (*p) {
        unsigned int c;
        if (*p < 0x80u) {
            c = *p++;
        } else if ((*p & 0xE0u) == 0xC0u) {
            c = (unsigned int)(*p++ & 0x1Fu) << 6;
            if (*p) c |= (unsigned int)(*p++ & 0x3Fu);
        } else if ((*p & 0xF0u) == 0xE0u) {
            c = (unsigned int)(*p++ & 0x0Fu) << 12;
            if (*p) { c |= (unsigned int)(*p++ & 0x3Fu) << 6; if (*p) c |= (unsigned int)(*p++ & 0x3Fu); }
        } else {
            c = (unsigned int)(*p++ & 0x07u) << 18;
            if (*p) {
                c |= (unsigned int)(*p++ & 0x3Fu) << 12;
                if (*p) { c |= (unsigned int)(*p++ & 0x3Fu) << 6; if (*p) c |= (unsigned int)(*p++ & 0x3Fu); }
            }
        }
        if (c >= 0x10000u) {
            if (n + 2 > cap) break;
            c -= 0x10000u;
            buf[n++] = (jchar)(0xD800u + (c >> 10));
            buf[n++] = (jchar)(0xDC00u + (c & 0x3FFu));
        } else {
            if (n + 1 > cap) break;
            buf[n++] = (jchar)c;
        }
    }
    ret = (*env)->NewString(env, buf, (jsize)n);
    free(buf);
    return ret;
}

JNIEXPORT jstring JNICALL
Java_com_sai_app_Native_call(JNIEnv *env, jclass cls, jstring jreq)
{
    char *req;
    sb out;
    jstring ret;

    (void)cls;
    if (!jreq) return NULL;
    req = jstr_to_utf8(env, jreq);
    if (!req) return NULL;
    sb_init(&out);
    handle(req, strlen(req), &out);
    free(req);
    ret = utf8_to_jstr(env, sb_cstr(&out));
    sb_free(&out);
    return ret;
}

/* feed(handle, chunk)：流式读取的每一块都走这里。
 * 用 byte[] 而不是 String，避免在网络分块处切断多字节字符。
 * 句柄只用于日志与错配排查：C 侧同一时刻只允许一条活动流（sendBegin 会拒绝并发）。 */
JNIEXPORT jstring JNICALL
Java_com_sai_app_Native_feed(JNIEnv *env, jclass cls, jint handle_no, jbyteArray chunk)
{
    sb out;
    jstring ret;
    jsize n;
    unsigned char stackbuf[4096];

    (void)cls;
    sb_init(&out);
    n = chunk ? (*env)->GetArrayLength(env, chunk) : 0;
    if (n <= 0) {
        if (handle_no > 0 && handle_no != g_gen.handle) reply_err(&out, "回复句柄已失效");
        else gen_feed_data("", 0, &out);
    } else if ((size_t)n <= sizeof(stackbuf)) {
        (*env)->GetByteArrayRegion(env, chunk, 0, n, (jbyte *)stackbuf);
        gen_feed_data(stackbuf, (size_t)n, &out);
    } else {
        jbyte *bytes = (*env)->GetByteArrayElements(env, chunk, NULL);
        if (!bytes) {
            reply_err(&out, "读取网络数据失败");
        } else {
            gen_feed_data(bytes, (size_t)n, &out);
            (*env)->ReleaseByteArrayElements(env, chunk, bytes, JNI_ABORT);
        }
    }
    ret = utf8_to_jstr(env, sb_cstr(&out));
    sb_free(&out);
    return ret;
}

JNIEXPORT void JNICALL
Java_com_sai_app_Native_shutdown(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    gen_lock();
    if (g_gen.active) {
        g_gen.cancel = 1;
        if (g_eng) aic_engine_stop(g_eng);
    }
    gen_reset();
    gen_unlock();
    close_env();
    if (g_gen_ready) {
        sb_free(&g_gen.text);
        sb_free(&g_gen.body);
        g_gen_ready = 0;
    }
}
