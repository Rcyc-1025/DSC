/* engine.c —— 对话引擎实现。 */
#include "engine.h"

#include "diaglog.h"
#include "prompt.h"
#include "store_conversation.h"
#include "store_message.h"
#include "store_persona.h"
#include "store_profile.h"
#include "store_setting.h"
#include "store_usage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aic_engine {
    aic_db *db;
    aic_llm_transport tr;
    char endpoint[AIC_LLM_URL_MAX];
    char api_key[256];
    char plugin_fragment[4096];
    char model_override[AIC_MODEL_BYTES];
    int  context_messages;
    int  stop_requested;
    /* 流式收集状态 */
    sb   stream_text;
    long long stream_pt;
    long long stream_ct;
    aic_llm_err stream_err;
    char stream_err_msg[256];
    int  stream_events;
    /* 流式增量回调：界面在工作线程里收到增量后 PostMessage 回界面线程渲染。 */
    void (*stream_cb)(void *ud, const char *delta);
    void *stream_cb_ud;
};

int aic_engine_create(aic_db *db, aic_engine **out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_engine *e = (aic_engine *)aic_xcalloc(1, sizeof(aic_engine));
    e->db = db;
    e->tr = aic_llm_transport_default();
    aic_strlcpy_utf8(e->endpoint, sizeof(e->endpoint), AIC_LLM_MODEL_DEFAULT_URL);
    e->context_messages = AIC_CONTEXT_MESSAGES_DEFAULT;
    e->stop_requested = 0;
    e->stream_err = AIC_LLM_OK;
    sb_init(&e->stream_text);
    *out = e;
    return AIC_OK;
}

void aic_engine_destroy(aic_engine *e)
{
    if (!e) return;
    sb_free(&e->stream_text);
    aic_xfree(e);
}

void aic_engine_set_transport(aic_engine *e, aic_llm_transport tr)
{
    if (e) e->tr = tr;
}

void aic_engine_set_endpoint(aic_engine *e, const char *url)
{
    if (!e || !url) return;
    aic_strlcpy_utf8(e->endpoint, sizeof(e->endpoint), url);
}

void aic_engine_set_api_key(aic_engine *e, const char *key)
{
    if (!e) return;
    aic_strlcpy_utf8(e->api_key, sizeof(e->api_key), key ? key : "");
}

int aic_engine_has_key(const aic_engine *e)
{
    return e && e->api_key[0] ? 1 : 0;
}

void aic_engine_set_context_messages(aic_engine *e, int n)
{
    if (!e) return;
    e->context_messages = n > 0 ? n : AIC_CONTEXT_MESSAGES_DEFAULT;
}

void aic_engine_set_plugin_fragment(aic_engine *e, const char *fragment)
{
    if (!e) return;
    aic_strlcpy_utf8(e->plugin_fragment, sizeof(e->plugin_fragment), fragment ? fragment : "");
}

void aic_engine_set_model_override(aic_engine *e, const char *model_id)
{
    if (!e) return;
    aic_strlcpy_utf8(e->model_override, sizeof(e->model_override), model_id ? model_id : "");
}

void aic_engine_set_stream_cb(aic_engine *e, void (*fn)(void *ud, const char *delta), void *ud)
{
    if (!e) return;
    e->stream_cb = fn;
    e->stream_cb_ud = ud;
}

void aic_engine_stop(aic_engine *e)
{
    if (e) e->stop_requested = 1;
}

int aic_engine_is_stopped(const aic_engine *e)
{
    return e && e->stop_requested ? 1 : 0;
}

/* 组装系统提示词：人设 + 资料 + 插件片段（顺序固定，见 prompt.c）。 */
static int build_system_prompt(aic_engine *e, const char *conv_id, sb *out,
                               char *err, size_t errlen)
{
    aic_conversation conv;
    int rc = aic_conv_get(e->db, conv_id, &conv);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", aic_strerror(rc));
        return rc;
    }
    aic_persona persona;
    memset(&persona, 0, sizeof(persona));
    if (conv.persona_id[0]) {
        if (aic_persona_get(e->db, conv.persona_id, &persona) != AIC_OK) {
            aic_persona_ensure_default(e->db, &persona);
        }
    } else {
        aic_persona_ensure_default(e->db, &persona);
    }
    aic_profile profile;
    memset(&profile, 0, sizeof(profile));
    aic_profile_get(e->db, &profile);

    rc = aic_prompt_build(&persona, &profile, e->plugin_fragment[0] ? e->plugin_fragment : NULL, out);
    aic_xfree(persona.base_prompt);
    return rc;
}

static const char *effective_model(aic_engine *e, const aic_conversation *conv)
{
    if (e->model_override[0]) return e->model_override;
    if (conv && conv->model_id[0]) return conv->model_id;
    return AIC_MODEL_FLASH;
}

int aic_engine_build_request(aic_engine *e, const char *conv_id, sb *out, char *err, size_t errlen)
{
    if (!e || !conv_id || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    aic_conversation conv;
    int rc = aic_conv_get(e->db, conv_id, &conv);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", aic_strerror(rc));
        return rc;
    }
    sb sys;
    sb_init(&sys);
    rc = build_system_prompt(e, conv_id, &sys, err, errlen);
    if (rc != AIC_OK) { sb_free(&sys); return rc; }

    aic_llm_request_opts opts = aic_llm_request_defaults();
    opts.context_messages = e->context_messages;
    rc = aic_llm_build_request(e->db, conv_id, effective_model(e, &conv), sb_cstr(&sys), &opts,
                               out, err, errlen);
    sb_free(&sys);
    return rc;
}

/* ---------- 流式回调 ---------- */
static void engine_on_event(void *ud, const aic_llm_event *ev)
{
    aic_engine *e = (aic_engine *)ud;
    e->stream_events++;
    switch (ev->kind) {
    case AIC_LLM_EV_DELTA:
        if (ev->text) {
            sb_append(&e->stream_text, ev->text);
            if (e->stream_cb) e->stream_cb(e->stream_cb_ud, ev->text);
        }
        break;
    case AIC_LLM_EV_USAGE:
        if (ev->prompt_tokens) e->stream_pt = ev->prompt_tokens;
        if (ev->completion_tokens) e->stream_ct = ev->completion_tokens;
        break;
    case AIC_LLM_EV_ERROR:
        e->stream_err = ev->err;
        aic_strlcpy_utf8(e->stream_err_msg, sizeof(e->stream_err_msg), ev->message);
        break;
    default:
        break;
    }
}

static long long estimate_tokens(const char *s)
{
    if (!s) return 0;
    /* 粗略估算：中文约 1 字 1 token，英文约 4 字节 1 token；只用于界面提示。 */
    size_t bytes = strlen(s);
    size_t chars = aic_utf8_length(s);
    long long est = (long long)chars + (long long)(bytes > chars ? (bytes - chars) / 4 : 0);
    return est;
}

/* 组装请求 → 流式收集 → 落助手消息 → 用量入账。
 * send 与 regenerate 共用这一段（区别只在于是否先追加用户消息）。 */
static int run_stream(aic_engine *e, const char *conv_id, const aic_conversation *conv,
                      aic_engine_result *local, char *err, size_t errlen)
{
    sb body;
    sb_init(&body);
    int rc = aic_engine_build_request(e, conv_id, &body, err, errlen);
    if (rc != AIC_OK) { sb_free(&body); return rc; }

    e->stop_requested = 0;
    e->stream_err = AIC_LLM_OK;
    e->stream_err_msg[0] = '\0';
    e->stream_pt = 0;
    e->stream_ct = 0;
    e->stream_events = 0;
    sb_reset(&e->stream_text);

    aic_sse_parser parser;
    aic_sse_init(&parser);
    aic_llm_err lerr = e->tr.post_stream(e->tr.ud, e->endpoint, e->api_key, sb_cstr(&body),
                                        &parser, engine_on_event, e, &e->stop_requested,
                                        err, errlen);
    if (lerr == AIC_LLM_OK) {
        aic_sse_finish(&parser, engine_on_event, e);
    }
    if (e->stream_err != AIC_LLM_OK && lerr == AIC_LLM_OK) lerr = e->stream_err;

    if (lerr != AIC_LLM_OK) {
        /* 传输失败：保留用户消息（草稿不丢），不写空的助手消息；解析失败把原始响应留进诊断日志。 */
        local->llm_err = lerr;
        if (lerr == AIC_LLM_E_PARSE && parser.raw.data && parser.raw.len) {
            aic_log_blob("模型流式响应（解析失败）", parser.raw.data, parser.raw.len);
        }
        aic_log_write("WARN", "模型请求失败：%s（%d）", aic_llm_err_text(lerr), (int)lerr);
        if (err && errlen && !err[0]) aic_strlcpy_utf8(err, errlen, aic_llm_err_text(lerr));
        sb_free(&body);
        aic_sse_free(&parser);
        return lerr == AIC_LLM_E_AUTH ? AIC_E_AUTH
             : (lerr == AIC_LLM_E_CANCEL ? AIC_E_STATE : AIC_E_TIMEOUT);
    }

    /* 落助手消息。 */
    aic_message amsg;
    memset(&amsg, 0, sizeof(amsg));
    rc = aic_msg_append(e->db, conv_id, AIC_ROLE_ASSISTANT, sb_cstr(&e->stream_text),
                        effective_model(e, (aic_conversation *)conv), &amsg);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "保存回复失败：%s", aic_strerror(rc));
        sb_free(&body);
        aic_sse_free(&parser);
        return rc;
    }
    aic_strlcpy_utf8(local->assistant_msg_id, sizeof(local->assistant_msg_id), amsg.id);
    aic_msg_free(&amsg);

    /* 用量与费用入账（余额并列展示的数据来源之一）。 */
    long long pt = e->stream_pt, ct = e->stream_ct;
    if (pt <= 0) pt = estimate_tokens(sb_cstr(&body));
    if (ct <= 0) ct = estimate_tokens(sb_cstr(&e->stream_text));
    const aic_llm_model *model = aic_llm_model_or_default(effective_model(e, (aic_conversation *)conv));
    long long cost = aic_llm_cost_micros(model, pt, ct);
    aic_usage_append(e->db, conv_id, local->assistant_msg_id, model->id, pt, ct, cost);

    local->text = aic_strdup(sb_cstr(&e->stream_text));
    local->prompt_tokens = pt;
    local->completion_tokens = ct;
    local->cost_micros = cost;
    local->llm_err = AIC_LLM_OK;
    sb_free(&body);
    aic_sse_free(&parser);
    return AIC_OK;
}

int aic_engine_send(aic_engine *e, const char *conv_id, const char *user_text,
                    aic_engine_result *res, char *err, size_t errlen)
{
    if (!e || !conv_id || !user_text) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    aic_engine_result local;
    memset(&local, 0, sizeof(local));
    local.llm_err = AIC_LLM_OK;

    aic_conversation conv;
    int rc = aic_conv_get(e->db, conv_id, &conv);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", aic_strerror(rc));
        return rc;
    }

    /* 1) 落用户消息（失败也不丢草稿：调用方拿到的还是原文）。 */
    aic_message umsg;
    memset(&umsg, 0, sizeof(umsg));
    rc = aic_msg_append(e->db, conv_id, AIC_ROLE_USER, user_text,
                        effective_model(e, &conv), &umsg);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "保存消息失败：%s", aic_strerror(rc));
        return rc;
    }
    aic_strlcpy_utf8(local.user_msg_id, sizeof(local.user_msg_id), umsg.id);
    aic_msg_free(&umsg);

    /* 2) 组装请求并走传输层，最后落库与记账。 */
    int rc2 = run_stream(e, conv_id, &conv, &local, err, errlen);
    if (res) *res = local;
    return rc2;
}

int aic_engine_regenerate(aic_engine *e, const char *conv_id, char *out_old_id, size_t old_cap,
                          aic_engine_result *res, char *err, size_t errlen)
{
    if (!e || !conv_id) return AIC_E_INVAL;
    if (out_old_id && old_cap) out_old_id[0] = '\0';
    if (err && errlen) err[0] = '\0';

    aic_conversation conv;
    int rc = aic_conv_get(e->db, conv_id, &conv);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", aic_strerror(rc));
        return rc;
    }
    /* 把上一条回复标记为「已被替换」：数据保留可回看，上下文与列表不再使用它。 */
    aic_message old;
    memset(&old, 0, sizeof(old));
    if (aic_msg_last_assistant(e->db, conv_id, &old) != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "没有可重新生成的回复");
        return AIC_E_STATE;
    }
    if (out_old_id && old_cap) aic_strlcpy_utf8(out_old_id, old_cap, old.id);
    rc = aic_msg_supersede(e->db, old.id);
    aic_msg_free(&old);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "标记旧回复失败：%s", aic_strerror(rc));
        return rc;
    }

    aic_engine_result local;
    memset(&local, 0, sizeof(local));
    local.llm_err = AIC_LLM_OK;
    rc = run_stream(e, conv_id, &conv, &local, err, errlen);
    if (res) *res = local;
    return rc;
}

int aic_engine_can_recall(aic_engine *e, const char *msg_id, int window_seconds)
{
    if (!e || !msg_id) return 0;
    if (window_seconds < 0) window_seconds = AIC_RECALL_WINDOW_DEFAULT;
    aic_message m;
    memset(&m, 0, sizeof(m));
    if (aic_msg_get(e->db, msg_id, &m) != AIC_OK) return 0;
    int ok = 0;
    if (!m.recalled) {
        int64_t created = aic_msg_created_ms(e->db, msg_id);
        int64_t now = aic_now_ms();
        if (created > 0 && now - created <= (int64_t)window_seconds * 1000) ok = 1;
    }
    aic_msg_free(&m);
    return ok;
}

int aic_engine_recall(aic_engine *e, const char *msg_id, int window_seconds,
                      char *err, size_t errlen)
{
    if (!e || !msg_id) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (!aic_engine_can_recall(e, msg_id, window_seconds)) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "已超过可撤回时间窗");
        return AIC_E_STATE;
    }
    return aic_msg_recall(e->db, msg_id);
}

int aic_engine_edit(aic_engine *e, const char *msg_id, const char *new_content,
                    char *err, size_t errlen)
{
    if (!e || !msg_id || !new_content) return AIC_E_INVAL;
    return aic_msg_edit(e->db, msg_id, new_content, err, errlen);
}

int aic_engine_context_stats(aic_engine *e, const char *conv_id, int *out_total,
                             int *out_used, long long *out_tokens_estimate)
{
    if (!e || !conv_id) return AIC_E_INVAL;
    long long total = 0;
    int rc = aic_msg_count(e->db, conv_id, &total);
    if (rc != AIC_OK) return rc;
    aic_message *msgs = NULL;
    size_t n = 0;
    long long est = 0;
    if (aic_msg_context(e->db, conv_id, e->context_messages, &msgs, &n) == AIC_OK) {
        for (size_t i = 0; i < n; i++) est += estimate_tokens(msgs[i].content);
        aic_msg_free_array(msgs, n);
    }
    if (out_total) *out_total = (int)total;
    if (out_used) *out_used = (int)n;
    if (out_tokens_estimate) *out_tokens_estimate = est;
    return AIC_OK;
}

void aic_engine_result_free(aic_engine_result *r)
{
    if (!r) return;
    aic_xfree(r->text);
    r->text = NULL;
}
