/* engine.c —— 对话引擎实现。 */
#include "engine.h"

#include "capability.h"
#include "diaglog.h"
#include "error_store.h"
#include "memory.h"
#include "sync.h"
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

struct dsc_engine {
    dsc_db *db;
    dsc_llm_transport tr;
    char endpoint[DSC_LLM_URL_MAX];
    char api_key[256];
    char plugin_fragment[4096];
    char model_override[DSC_MODEL_BYTES];
    int  context_messages;
    int  stop_requested;
    /* 流式收集状态 */
    sb   stream_text;
    long long stream_pt;
    long long stream_ct;
    dsc_llm_err stream_err;
    char stream_err_msg[256];
    int  stream_events;
    /* 流式增量回调：界面在工作线程里收到增量后 PostMessage 回界面线程渲染。 */
    void (*stream_cb)(void *ud, const char *delta);
    void *stream_cb_ud;
    /* 扩展点挂钩 */
    dsc_engine_text_fn transform;
    void *transform_ud;
    dsc_engine_tools_fn tools_provider;
    void *tools_ud;
    sb tools_json;
    /* FR-34 / FR-35 / FR-36 */
    char data_root[512];
    char origin_device[40];
    int  memory_enabled;
    int  capability_enabled;
    dsc_sync *sync;
};

int dsc_engine_create(dsc_db *db, dsc_engine **out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_engine *e = (dsc_engine *)dsc_xcalloc(1, sizeof(dsc_engine));
    e->db = db;
    e->tr = dsc_llm_transport_default();
    dsc_strlcpy_utf8(e->endpoint, sizeof(e->endpoint), DSC_LLM_MODEL_DEFAULT_URL);
    e->context_messages = DSC_CONTEXT_MESSAGES_DEFAULT;
    e->stop_requested = 0;
    e->stream_err = DSC_LLM_OK;
    sb_init(&e->stream_text);
    sb_init(&e->tools_json);
    e->memory_enabled = 1;
    e->capability_enabled = 1;
    *out = e;
    return DSC_OK;
}

void dsc_engine_destroy(dsc_engine *e)
{
    if (!e) return;
    sb_free(&e->stream_text);
    sb_free(&e->tools_json);
    dsc_xfree(e);
}

void dsc_engine_set_transport(dsc_engine *e, dsc_llm_transport tr)
{
    if (e) e->tr = tr;
}

void dsc_engine_set_endpoint(dsc_engine *e, const char *url)
{
    if (!e || !url) return;
    dsc_strlcpy_utf8(e->endpoint, sizeof(e->endpoint), url);
}

void dsc_engine_set_api_key(dsc_engine *e, const char *key)
{
    if (!e) return;
    dsc_strlcpy_utf8(e->api_key, sizeof(e->api_key), key ? key : "");
}

int dsc_engine_has_key(const dsc_engine *e)
{
    return e && e->api_key[0] ? 1 : 0;
}

void dsc_engine_set_context_messages(dsc_engine *e, int n)
{
    if (!e) return;
    e->context_messages = n > 0 ? n : DSC_CONTEXT_MESSAGES_DEFAULT;
}

void dsc_engine_set_plugin_fragment(dsc_engine *e, const char *fragment)
{
    if (!e) return;
    dsc_strlcpy_utf8(e->plugin_fragment, sizeof(e->plugin_fragment), fragment ? fragment : "");
}

void dsc_engine_set_model_override(dsc_engine *e, const char *model_id)
{
    if (!e) return;
    dsc_strlcpy_utf8(e->model_override, sizeof(e->model_override), model_id ? model_id : "");
}

void dsc_engine_set_stream_cb(dsc_engine *e, void (*fn)(void *ud, const char *delta), void *ud)
{
    if (!e) return;
    e->stream_cb = fn;
    e->stream_cb_ud = ud;
}

void dsc_engine_set_transform(dsc_engine *e, dsc_engine_text_fn fn, void *ud)
{
    if (!e) return;
    e->transform = fn;
    e->transform_ud = ud;
}

void dsc_engine_set_tools_provider(dsc_engine *e, dsc_engine_tools_fn fn, void *ud)
{
    if (!e) return;
    e->tools_provider = fn;
    e->tools_ud = ud;
}

void dsc_engine_set_data_root(dsc_engine *e, const char *path)
{
    if (!e) return;
    dsc_strlcpy_utf8(e->data_root, sizeof(e->data_root), path ? path : "");
}

void dsc_engine_set_origin_device(dsc_engine *e, const char *device_id)
{
    if (!e) return;
    dsc_strlcpy_utf8(e->origin_device, sizeof(e->origin_device), device_id ? device_id : "");
}

void dsc_engine_set_sync(dsc_engine *e, dsc_sync *sync)
{
    if (e) e->sync = sync;
}

void dsc_engine_set_memory_enabled(dsc_engine *e, int on)
{
    if (e) e->memory_enabled = on ? 1 : 0;
}

void dsc_engine_set_capability_enabled(dsc_engine *e, int on)
{
    if (e) e->capability_enabled = on ? 1 : 0;
}

void dsc_engine_stop(dsc_engine *e)
{
    if (e) e->stop_requested = 1;
}

int dsc_engine_is_stopped(const dsc_engine *e)
{
    return e && e->stop_requested ? 1 : 0;
}

/* 组装系统提示词：人设 + 资料 + 插件片段（顺序固定，见 prompt.c）。 */
static int build_system_prompt(dsc_engine *e, const char *conv_id, sb *out,
                               char *err, size_t errlen)
{
    dsc_conversation conv;
    int rc = dsc_conv_get(e->db, conv_id, &conv);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", dsc_strerror(rc));
        return rc;
    }
    dsc_persona persona;
    memset(&persona, 0, sizeof(persona));
    if (conv.persona_id[0]) {
        if (dsc_persona_get(e->db, conv.persona_id, &persona) != DSC_OK) {
            dsc_persona_ensure_default(e->db, &persona);
        }
    } else {
        dsc_persona_ensure_default(e->db, &persona);
    }
    dsc_profile profile;
    memset(&profile, 0, sizeof(profile));
    dsc_profile_get(e->db, &profile);

    rc = dsc_prompt_build(&persona, &profile, e->plugin_fragment[0] ? e->plugin_fragment : NULL, out);
    dsc_xfree(persona.base_prompt);
    if (rc != DSC_OK) return rc;

    /* FR-36：把本会话已确认的 AI 记忆带进上下文（不进系统提示词的人设区，单独成段）。 */
    if (e->memory_enabled) {
        sb mem;
        sb_init(&mem);
        if (dsc_mem_context(e->db, conv_id, &mem) == DSC_OK && mem.len > 0) {
            sb_append(out, "\n\n");
            sb_append(out, sb_cstr(&mem));
        }
        sb_free(&mem);
    }
    /* FR-35：只注入用户逐项授权、且本次确实取到的设备能力片段。 */
    if (e->capability_enabled) {
        sb cap;
        sb_init(&cap);
        if (dsc_cap_build_context(e->db, e->data_root, &cap) == DSC_OK && cap.len > 0) {
            sb_append(out, "\n\n");
            sb_append(out, sb_cstr(&cap));
        }
        sb_free(&cap);
    }
    return rc;
}

static const char *effective_model(dsc_engine *e, const dsc_conversation *conv)
{
    if (e->model_override[0]) return e->model_override;
    if (conv && conv->model_id[0]) return conv->model_id;
    return DSC_MODEL_FLASH;
}

int dsc_engine_build_request(dsc_engine *e, const char *conv_id, sb *out, char *err, size_t errlen)
{
    if (!e || !conv_id || !out) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    dsc_conversation conv;
    int rc = dsc_conv_get(e->db, conv_id, &conv);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", dsc_strerror(rc));
        return rc;
    }
    sb sys;
    sb_init(&sys);
    rc = build_system_prompt(e, conv_id, &sys, err, errlen);
    if (rc != DSC_OK) { sb_free(&sys); return rc; }

    dsc_llm_request_opts opts = dsc_llm_request_defaults();
    opts.context_messages = e->context_messages;

    char transformed[8192];
    transformed[0] = '\0';
    if (e->transform) {
        dsc_message last;
        memset(&last, 0, sizeof(last));
        if (dsc_msg_last_user(e->db, conv_id, &last) == DSC_OK) {
            char terr[256];
            terr[0] = '\0';
            if (e->transform(e->transform_ud, last.content ? last.content : "", transformed,
                             sizeof(transformed), terr, sizeof(terr)) == DSC_OK && transformed[0])
                opts.last_user_text = transformed;
            dsc_msg_free(&last);
        }
    }
    if (e->tools_provider) {
        sb_reset(&e->tools_json);
        char terr[256];
        terr[0] = '\0';
        if (e->tools_provider(e->tools_ud, &e->tools_json, terr, sizeof(terr)) == DSC_OK &&
            e->tools_json.len > 0)
            opts.tools_json = sb_cstr(&e->tools_json);
    }

    rc = dsc_llm_build_request(e->db, conv_id, effective_model(e, &conv), sb_cstr(&sys), &opts,
                               out, err, errlen);
    sb_free(&sys);
    return rc;
}

/* ---------- 流式回调 ---------- */
static void engine_on_event(void *ud, const dsc_llm_event *ev)
{
    dsc_engine *e = (dsc_engine *)ud;
    e->stream_events++;
    switch (ev->kind) {
    case DSC_LLM_EV_DELTA:
        if (ev->text) {
            sb_append(&e->stream_text, ev->text);
            if (e->stream_cb) e->stream_cb(e->stream_cb_ud, ev->text);
        }
        break;
    case DSC_LLM_EV_USAGE:
        if (ev->prompt_tokens) e->stream_pt = ev->prompt_tokens;
        if (ev->completion_tokens) e->stream_ct = ev->completion_tokens;
        break;
    case DSC_LLM_EV_ERROR:
        e->stream_err = ev->err;
        dsc_strlcpy_utf8(e->stream_err_msg, sizeof(e->stream_err_msg), ev->message);
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
    size_t chars = dsc_utf8_length(s);
    long long est = (long long)chars + (long long)(bytes > chars ? (bytes - chars) / 4 : 0);
    return est;
}

/* 组装请求 → 流式收集 → 落助手消息 → 用量入账。
 * send 与 regenerate 共用这一段（区别只在于是否先追加用户消息）。 */
static int run_stream(dsc_engine *e, const char *conv_id, const dsc_conversation *conv,
                      dsc_engine_result *local, char *err, size_t errlen)
{
    sb body;
    sb_init(&body);
    int rc = dsc_engine_build_request(e, conv_id, &body, err, errlen);
    if (rc != DSC_OK) { sb_free(&body); return rc; }

    e->stop_requested = 0;
    e->stream_err = DSC_LLM_OK;
    e->stream_err_msg[0] = '\0';
    e->stream_pt = 0;
    e->stream_ct = 0;
    e->stream_events = 0;
    sb_reset(&e->stream_text);

    dsc_sse_parser parser;
    dsc_sse_init(&parser);
    dsc_llm_err lerr = e->tr.post_stream(e->tr.ud, e->endpoint, e->api_key, sb_cstr(&body),
                                        &parser, engine_on_event, e, &e->stop_requested,
                                        err, errlen);
    if (lerr == DSC_LLM_OK) {
        dsc_sse_finish(&parser, engine_on_event, e);
    }
    if (e->stream_err != DSC_LLM_OK && lerr == DSC_LLM_OK) lerr = e->stream_err;

    if (lerr != DSC_LLM_OK) {
        /* 传输失败：保留用户消息（草稿不丢），不写空的助手消息；解析失败把原始响应留进诊断日志。 */
        local->llm_err = lerr;
        if (lerr == DSC_LLM_E_PARSE && parser.raw.data && parser.raw.len) {
            dsc_log_blob("模型流式响应（解析失败）", parser.raw.data, parser.raw.len);
        }
        dsc_log_write("WARN", "模型请求失败：%s（%d）", dsc_llm_err_text(lerr), (int)lerr);
        /* FR-34：错误写独立的 error_record（原始响应脱敏后落 errors/），不进消息表、
         * 不进上下文、不进聊天框；用户取消不算错误，不记录。 */
        if (lerr != DSC_LLM_E_CANCEL) {
            const char *detail = NULL;
            if (lerr == DSC_LLM_E_PARSE && parser.raw.data && parser.raw.len) {
                detail = parser.raw.data;
            } else if (e->stream_err_msg[0]) {
                detail = e->stream_err_msg;
            } else if (err && err[0]) {
                detail = err;
            }
            dsc_error_record(e->db, e->data_root, dsc_error_category_from_llm((int)lerr),
                             dsc_error_code_from_llm((int)lerr), dsc_llm_err_text(lerr),
                             conv_id, NULL, detail, NULL);
        }
        if (err && errlen && !err[0]) dsc_strlcpy_utf8(err, errlen, dsc_llm_err_text(lerr));
        sb_free(&body);
        dsc_sse_free(&parser);
        return lerr == DSC_LLM_E_AUTH ? DSC_E_AUTH
             : (lerr == DSC_LLM_E_CANCEL ? DSC_E_STATE : DSC_E_TIMEOUT);
    }

    /* 落助手消息。 */
    dsc_message amsg;
    memset(&amsg, 0, sizeof(amsg));
    rc = dsc_msg_append(e->db, conv_id, DSC_ROLE_ASSISTANT, sb_cstr(&e->stream_text),
                        effective_model(e, (dsc_conversation *)conv), &amsg);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "保存回复失败：%s", dsc_strerror(rc));
        sb_free(&body);
        dsc_sse_free(&parser);
        return rc;
    }
    dsc_strlcpy_utf8(local->assistant_msg_id, sizeof(local->assistant_msg_id), amsg.id);
    /* FR-36：AI 发言是记忆的来源；抽不出来（比如全是客套）不影响对话。 */
    if (e->memory_enabled) {
        int added = 0;
        dsc_mem_extract(e->db, amsg.id, conv_id, sb_cstr(&e->stream_text),
                        e->origin_device[0] ? e->origin_device : NULL, &added);
    }
    dsc_msg_free(&amsg);

    /* 用量与费用入账（余额并列展示的数据来源之一）。 */
    long long pt = e->stream_pt, ct = e->stream_ct;
    if (pt <= 0) pt = estimate_tokens(sb_cstr(&body));
    if (ct <= 0) ct = estimate_tokens(sb_cstr(&e->stream_text));
    const dsc_llm_model *model = dsc_llm_model_or_default(effective_model(e, (dsc_conversation *)conv));
    long long cost = dsc_llm_cost_micros(model, pt, ct);
    dsc_usage_append(e->db, conv_id, local->assistant_msg_id, model->id, pt, ct, cost);

    local->text = dsc_strdup(sb_cstr(&e->stream_text));
    local->prompt_tokens = pt;
    local->completion_tokens = ct;
    local->cost_micros = cost;
    local->llm_err = DSC_LLM_OK;
    sb_free(&body);
    dsc_sse_free(&parser);
    return DSC_OK;
}

int dsc_engine_send(dsc_engine *e, const char *conv_id, const char *user_text,
                    dsc_engine_result *res, char *err, size_t errlen)
{
    if (!e || !conv_id || !user_text) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    dsc_engine_result local;
    memset(&local, 0, sizeof(local));
    local.llm_err = DSC_LLM_OK;

    dsc_conversation conv;
    int rc = dsc_conv_get(e->db, conv_id, &conv);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", dsc_strerror(rc));
        return rc;
    }

    /* 1) 落用户消息（失败也不丢草稿：调用方拿到的还是原文）。 */
    dsc_message umsg;
    memset(&umsg, 0, sizeof(umsg));
    rc = dsc_msg_append(e->db, conv_id, DSC_ROLE_USER, user_text,
                        effective_model(e, &conv), &umsg);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "保存消息失败：%s", dsc_strerror(rc));
        return rc;
    }
    dsc_strlcpy_utf8(local.user_msg_id, sizeof(local.user_msg_id), umsg.id);
    dsc_msg_free(&umsg);

    /* 2) 组装请求并走传输层，最后落库与记账。 */
    int rc2 = run_stream(e, conv_id, &conv, &local, err, errlen);
    if (res) *res = local;
    return rc2;
}

int dsc_engine_regenerate(dsc_engine *e, const char *conv_id, char *out_old_id, size_t old_cap,
                          dsc_engine_result *res, char *err, size_t errlen)
{
    if (!e || !conv_id) return DSC_E_INVAL;
    if (out_old_id && old_cap) out_old_id[0] = '\0';
    if (err && errlen) err[0] = '\0';

    dsc_conversation conv;
    int rc = dsc_conv_get(e->db, conv_id, &conv);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "会话不存在：%s", dsc_strerror(rc));
        return rc;
    }
    /* 把上一条回复标记为「已被替换」：数据保留可回看，上下文与列表不再使用它。 */
    dsc_message old;
    memset(&old, 0, sizeof(old));
    if (dsc_msg_last_assistant(e->db, conv_id, &old) != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "没有可重新生成的回复");
        return DSC_E_STATE;
    }
    if (out_old_id && old_cap) dsc_strlcpy_utf8(out_old_id, old_cap, old.id);
    rc = dsc_msg_supersede(e->db, old.id);
    dsc_msg_free(&old);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "标记旧回复失败：%s", dsc_strerror(rc));
        return rc;
    }

    dsc_engine_result local;
    memset(&local, 0, sizeof(local));
    local.llm_err = DSC_LLM_OK;
    rc = run_stream(e, conv_id, &conv, &local, err, errlen);
    if (res) *res = local;
    return rc;
}

int dsc_engine_can_recall(dsc_engine *e, const char *msg_id, int window_seconds)
{
    if (!e || !msg_id) return 0;
    if (window_seconds < 0) window_seconds = DSC_RECALL_WINDOW_DEFAULT;
    dsc_message m;
    memset(&m, 0, sizeof(m));
    if (dsc_msg_get(e->db, msg_id, &m) != DSC_OK) return 0;
    int ok = 0;
    if (!m.recalled) {
        int64_t created = dsc_msg_created_ms(e->db, msg_id);
        int64_t now = dsc_now_ms();
        if (created > 0 && now - created <= (int64_t)window_seconds * 1000) ok = 1;
    }
    dsc_msg_free(&m);
    return ok;
}

int dsc_engine_recall(dsc_engine *e, const char *msg_id, int window_seconds,
                      char *err, size_t errlen)
{
    if (!e || !msg_id) return DSC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    if (!dsc_engine_can_recall(e, msg_id, window_seconds)) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "已超过可撤回时间窗");
        return DSC_E_STATE;
    }
    return dsc_msg_recall(e->db, msg_id);
}

int dsc_engine_edit(dsc_engine *e, const char *msg_id, const char *new_content,
                    char *err, size_t errlen)
{
    if (!e || !msg_id || !new_content) return DSC_E_INVAL;
    return dsc_msg_edit(e->db, msg_id, new_content, err, errlen);
}

/* FR-36：AI 发言修改 + 记忆重建 + 跨端 memory_replace，全程一个事务。 */
int dsc_engine_edit_ai(dsc_engine *e, const char *msg_id, const char *new_content,
                       int *out_mem_removed, int *out_mem_added,
                       char *err, size_t errlen)
{
    if (out_mem_removed) *out_mem_removed = 0;
    if (out_mem_added) *out_mem_added = 0;
    if (err && errlen) err[0] = '\0';
    if (!e || !msg_id || !new_content) return DSC_E_INVAL;

    dsc_message m;
    memset(&m, 0, sizeof(m));
    int rc = dsc_msg_get(e->db, msg_id, &m);
    if (rc != DSC_OK) return rc;
    if (strcmp(m.role, dsc_role_name(DSC_ROLE_ASSISTANT)) != 0) {
        dsc_msg_free(&m);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "只能修改 AI 发言");
        return DSC_E_STATE;
    }
    char conv_id[40];
    dsc_strlcpy_utf8(conv_id, sizeof(conv_id), m.conversation_id);
    dsc_msg_free(&m);

    rc = dsc_db_begin(e->db);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "开启事务失败：%s", dsc_strerror(rc));
        return rc;
    }
    int removed = 0, added = 0;
    rc = dsc_msg_edit(e->db, msg_id, new_content, err, errlen);
    if (rc == DSC_OK) rc = dsc_mem_purge(e->db, msg_id, &removed);
    if (rc == DSC_OK) {
        rc = dsc_mem_extract(e->db, msg_id, conv_id, new_content,
                             e->origin_device[0] ? e->origin_device : NULL, &added);
    }
    if (rc != DSC_OK) {
        dsc_db_rollback(e->db);
        if (err && errlen && !err[0]) dsc_strlcpy_utf8(err, errlen, "修改 AI 发言失败");
        return rc;
    }
    rc = dsc_db_commit(e->db);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "提交失败：%s", dsc_strerror(rc));
        return rc;
    }
    if (out_mem_removed) *out_mem_removed = removed;
    if (out_mem_added) *out_mem_added = added;
    dsc_log_write("INFO", "AI 发言已修改，记忆重建：删除 %d 条、新增 %d 条", removed, added);

    /* 跨端：发 memory_replace 事件，让对端按同一规则重抽（§9.2）。
     * 发送失败不影响本地结果，事件队列会补发。 */
    if (e->sync) {
        sb payload;
        sb_init(&payload);
        jw w;
        jw_init(&w, &payload, 1);
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "source_message_id", msg_id);
        jw_put_str(&w, "conversation_id", conv_id);
        jw_put_str(&w, "content", new_content);
        jw_put_str(&w, "op", "memory_replace");
        jw_obj_end(&w);
        char evid[40];
        char serr[256];
        serr[0] = '\0';
        dsc_sync_publish(e->sync, DSC_ENT_MEMORY, DSC_OP_UPDATE, msg_id, sb_cstr(&payload),
                         evid, sizeof(evid), serr, sizeof(serr));
        sb_free(&payload);
    }
    return DSC_OK;
}

int dsc_engine_context_stats(dsc_engine *e, const char *conv_id, int *out_total,
                             int *out_used, long long *out_tokens_estimate)
{
    if (!e || !conv_id) return DSC_E_INVAL;
    long long total = 0;
    int rc = dsc_msg_count(e->db, conv_id, &total);
    if (rc != DSC_OK) return rc;
    dsc_message *msgs = NULL;
    size_t n = 0;
    long long est = 0;
    if (dsc_msg_context(e->db, conv_id, e->context_messages, &msgs, &n) == DSC_OK) {
        for (size_t i = 0; i < n; i++) est += estimate_tokens(msgs[i].content);
        dsc_msg_free_array(msgs, n);
    }
    if (out_total) *out_total = (int)total;
    if (out_used) *out_used = (int)n;
    if (out_tokens_estimate) *out_tokens_estimate = est;
    return DSC_OK;
}

void dsc_engine_result_free(dsc_engine_result *r)
{
    if (!r) return;
    dsc_xfree(r->text);
    r->text = NULL;
}
