/* llm.c —— 模型目录、请求组装、SSE 流式解析、错误分类、费用与余额解析。 */
#include "llm.h"
#include "store_message.h"
#include "store_setting.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 模型目录 ---------- */
/* 内置两项 + 配置文件追加项（同 id 覆盖），文本字段各有独立存储，避免悬空指针。 */
#define AIC_LLM_MODELS_MAX 16
#define AIC_LLM_MODEL_TEXT 128

typedef struct model_slot {
    aic_llm_model m;
    char id[AIC_LLM_MODEL_TEXT];
    char name[AIC_LLM_MODEL_TEXT];
    char summary[256];
} model_slot;

static model_slot g_slots[AIC_LLM_MODELS_MAX];
static size_t g_model_n;

static void model_set(model_slot *s, const char *id, const char *name, const char *summary,
                     long long ctx, long long pin, long long pout)
{
    aic_strlcpy_utf8(s->id, sizeof(s->id), id ? id : "");
    aic_strlcpy_utf8(s->name, sizeof(s->name), name ? name : "");
    aic_strlcpy_utf8(s->summary, sizeof(s->summary), summary ? summary : "");
    s->m.id = s->id;
    s->m.name = s->name;
    s->m.summary = s->summary;
    s->m.context_tokens = ctx;
    s->m.price_in_micro_per_mtok = pin;
    s->m.price_out_micro_per_mtok = pout;
}

static void ensure_models(void)
{
    if (g_model_n > 0) return;
    model_set(&g_slots[0], AIC_MODEL_FLASH, "DeepSeek V4.1 Flash",
              "短轮次快响应，日常陪伴对话默认用它", 65536, 2000000, 8000000);
    model_set(&g_slots[1], AIC_MODEL_PRO, "DeepSeek V4 Pro",
              "长文本与复杂推理，回答更慢但更稳", 131072, 4000000, 16000000);
    g_model_n = 2;
}

static aic_llm_model *find_mut(const char *id)
{
    ensure_models();
    if (!id || !id[0]) return NULL;
    for (size_t i = 0; i < g_model_n; i++) {
        if (strcmp(g_slots[i].id, id) == 0) return &g_slots[i].m;
    }
    return NULL;
}

size_t aic_llm_model_count(void)
{
    ensure_models();
    return g_model_n;
}

const aic_llm_model *aic_llm_model_at(size_t i)
{
    ensure_models();
    if (i >= g_model_n) return NULL;
    return &g_slots[i].m;
}

const aic_llm_model *aic_llm_model_find(const char *id)
{
    return find_mut(id);
}

const aic_llm_model *aic_llm_model_or_default(const char *id)
{
    ensure_models();
    const aic_llm_model *m = aic_llm_model_find(id);
    return m ? m : &g_slots[0].m;
}

void aic_llm_models_reset(void)
{
    g_model_n = 0;
    memset(g_slots, 0, sizeof(g_slots));
    ensure_models();
}

/* 配置文件追加：{"models":[{...}]} 或直接数组；同 id 覆盖已有项。 */
int aic_llm_models_load_json(const char *json, size_t len, char *err, size_t errlen)
{
    if (!json) return AIC_E_INVAL;
    ensure_models();
    jv *root = NULL;
    if (jv_parse(json, len, &root, err, errlen) != AIC_OK) { jv_free(root); return AIC_E_FORMAT; }

    const jv *arr = root;
    if (root && root->type == JV_OBJ) arr = jv_get(root, "models");
    if (!arr || arr->type != JV_ARR) {
        if (err && errlen) snprintf(err, errlen, "配置文件缺少 models 数组");
        jv_free(root);
        return AIC_E_FORMAT;
    }
    size_t added = 0;
    for (size_t i = 0; i < jv_len(arr); i++) {
        const jv *it = jv_at(arr, i);
        if (!it || it->type != JV_OBJ) continue;
        const char *id = jv_get_str(it, "id", NULL);
        if (!id || !id[0]) {
            if (err && errlen) snprintf(err, errlen, "第 %d 个模型缺少 id", (int)(i + 1));
            jv_free(root);
            return AIC_E_FORMAT;
        }
        aic_llm_model *exist = find_mut(id);
        model_slot *slot = NULL;
        if (exist) {
            slot = (model_slot *)((char *)exist - offsetof(model_slot, m));
        } else {
            if (g_model_n >= AIC_LLM_MODELS_MAX) {
                if (err && errlen) snprintf(err, errlen, "模型数量上限为 %d", AIC_LLM_MODELS_MAX);
                jv_free(root);
                return AIC_E_LIMIT;
            }
            slot = &g_slots[g_model_n++];
        }
        model_set(slot, id, jv_get_str(it, "name", id),
                  jv_get_str(it, "summary", ""),
                  jv_get_ll(it, "context_tokens", 65536),
                  jv_get_ll(it, "price_in_micro_per_mtok", 2000000),
                  jv_get_ll(it, "price_out_micro_per_mtok", 8000000));
        added++;
    }
    jv_free(root);
    if (added == 0 && err && errlen) snprintf(err, errlen, "配置文件里没有可用的模型项");
    return AIC_OK;
}

int aic_llm_models_load_file(const char *path, char *err, size_t errlen)
{
    if (!path) return AIC_E_INVAL;
    if (!aic_path_exists(path)) return AIC_E_NOTFOUND;
    char *data = NULL;
    size_t len = 0;
    int rc = aic_file_read_all(path, &data, &len);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "读取模型配置失败：%s", aic_strerror(rc));
        return rc;
    }
    rc = aic_llm_models_load_json(data, len, err, errlen);
    aic_xfree(data);
    return rc;
}

/* ---------- 费用 ---------- */
long long aic_llm_cost_micros(const aic_llm_model *m, long long prompt_tokens,
                              long long completion_tokens)
{
    if (!m) m = aic_llm_model_or_default(NULL);
    if (prompt_tokens < 0) prompt_tokens = 0;
    if (completion_tokens < 0) completion_tokens = 0;
    long long in_cost = 0, out_cost = 0;
    if (prompt_tokens > 0 && m->price_in_micro_per_mtok > 0) {
        in_cost = (prompt_tokens * m->price_in_micro_per_mtok + 999999) / 1000000;
        if (in_cost < 1) in_cost = 1;
    }
    if (completion_tokens > 0 && m->price_out_micro_per_mtok > 0) {
        out_cost = (completion_tokens * m->price_out_micro_per_mtok + 999999) / 1000000;
        if (out_cost < 1) out_cost = 1;
    }
    return in_cost + out_cost;
}

void aic_llm_format_micros(long long micros, char *dst, size_t cap)
{
    if (!dst || !cap) return;
    int neg = micros < 0;
    if (neg) micros = -micros;
    long long yuan = micros / 1000000;
    long long frac = (micros % 1000000) / 100; /* 保留 4 位小数 */
    snprintf(dst, cap, "%s¥%lld.%04lld", neg ? "-" : "", yuan, frac);
}

/* ---------- 错误分类 ---------- */
aic_llm_err aic_llm_classify_http(int http_status)
{
    if (http_status >= 200 && http_status < 300) return AIC_LLM_OK;
    if (http_status == 401 || http_status == 403) return AIC_LLM_E_AUTH;
    if (http_status == 402) return AIC_LLM_E_BALANCE;
    if (http_status == 429) return AIC_LLM_E_RATE;
    if (http_status == 404) return AIC_LLM_E_NOMODEL;
    if (http_status == 400 || http_status == 422) return AIC_LLM_E_BADREQ;
    if (http_status >= 500) return AIC_LLM_E_SERVER;
    if (http_status <= 0) return AIC_LLM_E_NETWORK;
    return AIC_LLM_E_NETWORK;
}

const char *aic_llm_err_text(aic_llm_err e)
{
    switch (e) {
    case AIC_LLM_OK:       return "正常";
    case AIC_LLM_E_NETWORK:return "网络异常，请检查网络后重试";
    case AIC_LLM_E_AUTH:   return "API Key 无效或已失效，请到设置里重新填写";
    case AIC_LLM_E_RATE:   return "请求过于频繁，已被模型限流，请稍后再试";
    case AIC_LLM_E_PARSE:  return "响应无法解析，已把原始响应写入诊断日志";
    case AIC_LLM_E_CANCEL: return "已停止生成";
    case AIC_LLM_E_SERVER: return "服务端暂时不可用，请稍后重试";
    case AIC_LLM_E_BADREQ: return "请求参数被拒绝，请检查模型与内容长度";
    case AIC_LLM_E_NOMODEL:return "指定的模型不存在，请在设置里重新选择";
    case AIC_LLM_E_BALANCE:return "账户余额不足，请充值后再试";
    default:               return "未知的模型错误";
    }
}

aic_llm_retry_policy aic_llm_retry_policy_default(void)
{
    aic_llm_retry_policy p;
    p.max_retries = 3;
    p.base_delay_ms = 500;
    p.max_delay_ms = 8000;
    return p;
}

int aic_llm_retry_delay(const aic_llm_retry_policy *p, aic_llm_err e, int attempt,
                        int *out_delay_ms)
{
    if (out_delay_ms) *out_delay_ms = 0;
    aic_llm_retry_policy def = aic_llm_retry_policy_default();
    if (!p) p = &def;
    /* 只有网络异常与服务端错误可以自动重试；限流与凭证失效都不重试。 */
    if (e != AIC_LLM_E_NETWORK && e != AIC_LLM_E_SERVER) return 0;
    if (attempt < 0) attempt = 0;
    if (attempt >= p->max_retries) return 0;
    long long delay = (long long)p->base_delay_ms << (attempt > 20 ? 20 : attempt);
    if (delay > p->max_delay_ms) delay = p->max_delay_ms;
    if (delay < 1) delay = 1;
    if (out_delay_ms) *out_delay_ms = (int)delay;
    return 1;
}

/* ---------- 请求组装 ---------- */
aic_llm_request_opts aic_llm_request_defaults(void)
{
    aic_llm_request_opts o;
    memset(&o, 0, sizeof(o));   /* 指针字段必须清零：last_user_text / tools_json 未用即 NULL */
    o.stream = 1;
    o.temperature = 0.7;
    o.max_tokens = 0;
    o.context_messages = AIC_CONTEXT_MESSAGES_DEFAULT;
    return o;
}

int aic_llm_build_request(aic_db *db, const char *conv_id, const char *model_id,
                          const char *system_prompt, const aic_llm_request_opts *opts,
                          sb *out, char *err, size_t errlen)
{
    if (!db || !conv_id || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    aic_llm_request_opts def = aic_llm_request_defaults();
    if (!opts) opts = &def;
    if (!model_id || !model_id[0]) model_id = AIC_MODEL_FLASH;
    if (!aic_llm_model_find(model_id)) {
        if (err && errlen) snprintf(err, errlen, "模型 %s 不在目录中", model_id);
        return AIC_E_INVAL;
    }

    aic_message *msgs = NULL;
    size_t n = 0;
    int limit = opts->context_messages > 0 ? opts->context_messages : AIC_CONTEXT_MESSAGES_DEFAULT;
    int rc = aic_msg_context(db, conv_id, limit, &msgs, &n);
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "读取上下文失败：%s", aic_strerror(rc));
        return rc;
    }

    jw w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "model", model_id);
    jw_arr_begin(&w, "messages");
    if (system_prompt && system_prompt[0]) {
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "role", "system");
        jw_put_str(&w, "content", system_prompt);
        jw_obj_end(&w);
    }
    for (size_t i = 0; i < n; i++) {
        const char *role = msgs[i].role;
        if (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0 && strcmp(role, "system") != 0)
            continue;
        const char *content = msgs[i].content ? msgs[i].content : "";
        /* message.transform：只替换最后一条 user 消息（库里的原文不动）。 */
        if (opts->last_user_text && opts->last_user_text[0] && i + 1 == n &&
            strcmp(role, "user") == 0)
            content = opts->last_user_text;
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "role", role);
        jw_put_str(&w, "content", content);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_put_bool(&w, "stream", opts->stream ? 1 : 0);
    if (opts->temperature >= 0.0) jw_put_double(&w, "temperature", opts->temperature);
    if (opts->max_tokens > 0) jw_put_int(&w, "max_tokens", opts->max_tokens);
    if (opts->tools_json && opts->tools_json[0]) jw_put_raw(&w, "tools", opts->tools_json);
    jw_obj_end(&w);

    aic_msg_free_array(msgs, n);
    return AIC_OK;
}

/* ---------- SSE 解析 ---------- */
void aic_sse_init(aic_sse_parser *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    sb_init(&p->buf);
    sb_init(&p->raw);
}

void aic_sse_free(aic_sse_parser *p)
{
    if (!p) return;
    sb_free(&p->buf);
    sb_free(&p->raw);
    memset(p, 0, sizeof(*p));
}

static void emit_simple(aic_llm_event_fn fn, void *ud, aic_llm_event_kind kind,
                        const char *text, aic_llm_err e, const char *msg,
                        long long pt, long long ct)
{
    if (!fn) return;
    aic_llm_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.text = text;
    ev.err = e;
    ev.prompt_tokens = pt;
    ev.completion_tokens = ct;
    if (msg) aic_strlcpy_utf8(ev.message, sizeof(ev.message), msg);
    fn(ud, &ev);
}

static long long pick_ll(const jv *o, const char *k)
{
    if (!o) return 0;
    const jv *v = jv_get(o, k);
    if (!v) return 0;
    if (v->type == JV_NUM) return (long long)jv_number(v);
    if (v->type == JV_STR) return aic_parse_ll(jv_string(v), 0);
    return 0;
}

/* 处理一条已经剥掉换行符的 SSE 行。 */
static int process_line(aic_sse_parser *p, const char *line, aic_llm_event_fn fn, void *ud)
{
    if (!line) return AIC_LLM_OK;
    /* 跳过空行与注释行（':' 开头是 SSE 注释/心跳）。 */
    const char *s = line;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (!*s || *s == ':') return AIC_LLM_OK;

    const char *payload = NULL;
    if (strncmp(s, "data:", 5) == 0) {
        payload = s + 5;
        while (*payload == ' ') payload++;
    } else if (strncmp(s, "event:", 6) == 0) {
        /* 事件名单独处理；本协议只依赖 data 里的 JSON。 */
        return AIC_LLM_OK;
    } else {
        payload = s; /* 有些服务端直接吐 JSON 行 */
    }

    if (strcmp(payload, "[DONE]") == 0) {
        p->saw_done = 1;
        p->finished = 1;
        emit_simple(fn, ud, AIC_LLM_EV_DONE, NULL, AIC_LLM_OK, NULL, 0, 0);
        return AIC_LLM_OK;
    }

    jv *root = NULL;
    char perr[256];
    perr[0] = '\0';
    if (jv_parse(payload, strlen(payload), &root, perr, sizeof(perr)) != AIC_OK) {
        char msg[300];
        snprintf(msg, sizeof(msg), "响应片段不是合法 JSON：%.120s", payload);
        emit_simple(fn, ud, AIC_LLM_EV_ERROR, NULL, AIC_LLM_E_PARSE, msg, 0, 0);
        return AIC_LLM_E_PARSE;
    }

    const jv *err_obj = jv_get(root, "error");
    if (err_obj) {
        const char *m = jv_get_str(err_obj, "message", "服务端返回错误");
        char code[64];
        aic_strlcpy_utf8(code, sizeof(code), jv_get_str(err_obj, "type", ""));
        aic_llm_err kind = strstr(code, "auth") ? AIC_LLM_E_AUTH
                         : (strstr(code, "rate") ? AIC_LLM_E_RATE : AIC_LLM_E_SERVER);
        emit_simple(fn, ud, AIC_LLM_EV_ERROR, NULL, kind, m, 0, 0);
        jv_free(root);
        return kind;
    }

    const jv *usage = jv_get(root, "usage");
    if (usage && usage->type == JV_OBJ) {
        long long pt = pick_ll(usage, "prompt_tokens");
        long long ct = pick_ll(usage, "completion_tokens");
        if (pt || ct) emit_simple(fn, ud, AIC_LLM_EV_USAGE, NULL, AIC_LLM_OK, NULL, pt, ct);
    }

    const jv *choices = jv_get(root, "choices");
    if (choices && choices->type == JV_ARR) {
        const jv *first = jv_at(choices, 0);
        if (first) {
            const jv *delta = jv_get(first, "delta");
            const jv *dcontent = delta ? jv_get(delta, "content") : NULL;
            if (!dcontent) {
                /* 有些兼容实现把增量放在 message.content 里。 */
                const jv *msgobj = jv_get(first, "message");
                dcontent = msgobj ? jv_get(msgobj, "content") : NULL;
            }
            if (dcontent && dcontent->type == JV_STR) {
                const char *txt = jv_string(dcontent);
                if (txt && txt[0]) {
                    p->delta_count++;
                    emit_simple(fn, ud, AIC_LLM_EV_DELTA, txt, AIC_LLM_OK, NULL, 0, 0);
                }
            }
            /* finish_reason 只表示本次回答结束，流的结束仍以 [DONE] 或连接关闭为准。 */
        }
    }
    jv_free(root);
    return AIC_LLM_OK;
}

int aic_sse_feed(aic_sse_parser *p, const char *data, size_t len,
                 aic_llm_event_fn fn, void *ud)
{
    if (!p || (!data && len)) return AIC_E_INVAL;
    if (p->finished) return AIC_LLM_OK;
    sb_appendn(&p->buf, data, len);
    sb_appendn(&p->raw, data, len);

    int rc = AIC_LLM_OK;
    for (;;) {
        const char *base = sb_cstr(&p->buf);
        size_t blen = p->buf.len;
        const char *nl = NULL;
        for (size_t i = 0; i < blen; i++) {
            if (base[i] == '\n') { nl = base + i; break; }
        }
        if (!nl) break;
        size_t line_len = (size_t)(nl - base);
        char *line = (char *)aic_xmalloc(line_len + 1);
        memcpy(line, base, line_len);
        line[line_len] = '\0';
        if (line_len && line[line_len - 1] == '\r') line[line_len - 1] = '\0';
        int r = process_line(p, line, fn, ud);
        if (r < 0 && rc == AIC_LLM_OK) rc = r;
        aic_xfree(line);

        /* 消费掉这一行。 */
        size_t rest = blen - line_len - 1;
        memmove(p->buf.data, base + line_len + 1, rest);
        p->buf.len = rest;
        p->buf.data[rest] = '\0';
        if (p->finished) break;
    }
    return rc;
}

int aic_sse_finish(aic_sse_parser *p, aic_llm_event_fn fn, void *ud)
{
    if (!p) return AIC_E_INVAL;
    if (p->buf.len) {
        char *line = (char *)aic_xmalloc(p->buf.len + 1);
        memcpy(line, sb_cstr(&p->buf), p->buf.len);
        line[p->buf.len] = '\0';
        if (p->buf.len && line[p->buf.len - 1] == '\r') line[p->buf.len - 1] = '\0';
        process_line(p, line, fn, ud);
        aic_xfree(line);
        p->buf.len = 0;
        if (p->buf.data) p->buf.data[0] = '\0';
    }
    if (!p->saw_done) {
        emit_simple(fn, ud, AIC_LLM_EV_DONE, NULL, AIC_LLM_OK, NULL, 0, 0);
        p->saw_done = 1;
    }
    p->finished = 1;
    return AIC_LLM_OK;
}

/* ---------- 默认传输层（明确不可用） ---------- */
aic_llm_err aic_llm_transport_unavailable_post(void *ud, const char *url, const char *api_key,
                                               const char *body, aic_sse_parser *parser,
                                               aic_llm_event_fn fn, void *cb_ud,
                                               volatile int *cancel, char *err, size_t errlen)
{
    (void)ud; (void)url; (void)api_key; (void)body; (void)parser; (void)fn; (void)cb_ud;
    if (err && errlen) {
        aic_strlcpy_utf8(err, errlen,
            "当前构建未启用网络传输：本项目按裁剪范围只实现请求组装与流式解析，"
            "真实 HTTP 由各平台的传输实现提供");
    }
    return AIC_LLM_E_NETWORK;
}

aic_llm_err aic_llm_transport_unavailable_probe(void *ud, const char *url, const char *api_key,
                                                char *err, size_t errlen)
{
    (void)ud; (void)url; (void)api_key;
    if (err && errlen) {
        aic_strlcpy_utf8(err, errlen, "当前构建未启用网络传输，无法做连通性校验");
    }
    return AIC_LLM_E_NETWORK;
}

aic_llm_transport aic_llm_transport_default(void)
{
    aic_llm_transport t;
    t.ud = NULL;
    t.post_stream = aic_llm_transport_unavailable_post;
    t.probe = aic_llm_transport_unavailable_probe;
    return t;
}

/* ---------- 余额 ---------- */
static int money_to_micros(const jv *v, int64_t *out)
{
    if (!v || !out) return AIC_E_INVAL;
    if (v->type == JV_NUM) {
        double d = jv_number(v);
        if (d < 0) d = 0;
        *out = (int64_t)(d * 1000000.0 + 0.5);
        return AIC_OK;
    }
    if (v->type != JV_STR) return AIC_E_INVAL;
    const char *s = jv_string(v);
    if (!s || !*s) return AIC_E_INVAL;
    /* 形如 "110.00" / "0.5" 的金额字符串。 */
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    long long whole = 0, frac = 0;
    int seen_dot = 0, frac_digits = 0;
    for (; *s; s++) {
        if (*s == '.') { if (seen_dot) break; seen_dot = 1; continue; }
        if (*s < '0' || *s > '9') break;
        if (!seen_dot) whole = whole * 10 + (*s - '0');
        else if (frac_digits < 6) { frac = frac * 10 + (*s - '0'); frac_digits++; }
    }
    while (frac_digits < 6) { frac *= 10; frac_digits++; }
    long long v2 = whole * 1000000 + frac;
    if (neg) v2 = -v2;
    *out = v2;
    return AIC_OK;
}

int aic_llm_balance_parse(const char *json, size_t len, aic_llm_balance *out,
                          char *err, size_t errlen)
{
    if (!json || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    jv *root = NULL;
    char perr[256];
    perr[0] = '\0';
    int rc = jv_parse(json, len, &root, perr, sizeof(perr));
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "余额响应不是合法 JSON：%s", perr);
        return AIC_E_FORMAT;
    }

    int64_t avail = -1, pending = 0;
    /* DeepSeek 官方接口把金额放在 balance_infos[0] 里，形如：
       {"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"12.34"}]} */
    const jv *src = root;
    {
        const jv *infos = jv_get(root, "balance_infos");
        if (infos && jv_len(infos) > 0) {
            const jv *first = jv_at(infos, 0);
            if (first) src = first;
        }
    }
    static const char *avail_keys[] = { "total_balance", "available_balance", "balance",
                                        "available", "remaining" };
    for (size_t i = 0; i < sizeof(avail_keys) / sizeof(avail_keys[0]); i++) {
        const jv *v = jv_get(src, avail_keys[i]);
        int64_t got = 0;
        if (v && money_to_micros(v, &got) == AIC_OK) { avail = got; break; }
    }
    if (avail < 0) {
        /* 有些响应把余额拆成"赠送余额 + 充值余额"。 */
        const jv *g = jv_get(src, "granted_balance");
        const jv *t = jv_get(src, "topped_up_balance");
        int64_t gv = 0, tv = 0;
        int have = 0;
        if (g && money_to_micros(g, &gv) == AIC_OK) have = 1;
        if (t && money_to_micros(t, &tv) == AIC_OK) have = 1;
        if (have) avail = gv + tv;
    }
    if (avail < 0) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "余额响应里没有可识别的余额字段");
        jv_free(root);
        return AIC_E_FORMAT;
    }
    const jv *p = jv_get(src, "pending_balance");
    if (!p) p = jv_get(root, "pending_balance");
    if (!p) p = jv_get(src, "unsettled_amount");
    if (!p) p = jv_get(root, "unsettled_amount");
    int64_t pv = 0;
    if (p && money_to_micros(p, &pv) == AIC_OK) pending = pv;

    out->valid = 1;
    out->available_micros = avail;
    out->pending_micros = pending;
    out->queried_ms = aic_now_ms();
    out->stale = 0;
    aic_strlcpy_utf8(out->source, sizeof(out->source), "账户接口");
    jv_free(root);
    return AIC_OK;
}

void aic_llm_balance_keep_last(aic_llm_balance *out, const aic_llm_balance *last, int64_t now_ms)
{
    if (!out) return;
    int64_t local_used = out->local_used_micros;
    if (last) {
        *out = *last;
        out->local_used_micros = local_used;
    }
    out->valid = 0;
    out->stale = 1;
    out->queried_ms = now_ms;
    aic_strlcpy_utf8(out->source, sizeof(out->source), "上次结果");
}

int aic_llm_balance_below(const aic_llm_balance *b, int64_t threshold_micros)
{
    if (!b) return 0;
    if (threshold_micros <= 0) return 0;
    return b->available_micros < threshold_micros ? 1 : 0;
}
