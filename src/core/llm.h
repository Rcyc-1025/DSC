/* llm.h —— 模型接入层：模型目录、请求组装、流式分片解析、错误分类、用量与费用。
 *
 * 说明（与产品裁决一致）：本仓库只实现"协议与组装"这一层，
 * 真实 HTTP 传输通过 aic_llm_transport 注入，默认实现返回 AIC_LLM_E_NETWORK，
 * 因此核心层可以在没有网络、没有 API Key 的情况下被完整测试（FR-02/FR-03）。
 */
#ifndef AIC_LLM_H
#define AIC_LLM_H

#include <stddef.h>

#include "aic_types.h"
#include "db.h"
#include "json.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_LLM_URL_MAX      512
#define AIC_LLM_MODEL_DEFAULT_URL "https://api.deepseek.com/chat/completions"

/* ---------- 模型目录（FR-03） ---------- */
typedef struct aic_llm_model {
    const char *id;             /* 配置里使用的模型标识 */
    const char *name;           /* 界面显示名 */
    const char *summary;        /* 一句话说明（何时选它） */
    long long   context_tokens; /* 上下文窗口 */
    long long   price_in_micro_per_mtok;  /* 输入价格：微元 / 百万 token */
    long long   price_out_micro_per_mtok; /* 输出价格：微元 / 百万 token */
} aic_llm_model;

/* 内置两个模型（deepseekv4.1flash / deepseekv4pro）。 */
size_t aic_llm_model_count(void);
const aic_llm_model *aic_llm_model_at(size_t i);
/* 找不到返回 NULL。 */
const aic_llm_model *aic_llm_model_find(const char *id);
/* 从配置文件追加模型（技术设计 9.3「新增模型通过配置文件追加，不改动核心代码」）。
 * 文件是 JSON：{"models":[{"id","name","summary","context_tokens",
 * "price_in_micro_per_mtok","price_out_micro_per_mtok"}]}，同 id 覆盖内置项。
 * 已加载的模型不会因为再次调用而重复追加。 */
int aic_llm_models_load_json(const char *json, size_t len, char *err, size_t errlen);
int aic_llm_models_load_file(const char *path, char *err, size_t errlen);
/* 恢复成只有两个内置模型（测试与「配置文件被删」时用）。 */
void aic_llm_models_reset(void);

/* 找不到时回退到默认模型（flash）。 */
const aic_llm_model *aic_llm_model_or_default(const char *id);

/* ---------- 费用（FR-11/FR-24） ---------- */
/* 微元（1 元 = 1,000,000 微元），向上取整到 1 微元。 */
long long aic_llm_cost_micros(const aic_llm_model *m, long long prompt_tokens,
                              long long completion_tokens);
/* 把微元格式化成"¥0.0123"这样的字符串。 */
void aic_llm_format_micros(long long micros, char *dst, size_t cap);

/* ---------- 错误分类（异常处理章） ---------- */
typedef enum aic_llm_err {
    AIC_LLM_OK = 0,
    AIC_LLM_E_NETWORK = -1,  /* 网络异常：指数退避重试 */
    AIC_LLM_E_AUTH    = -2,  /* 凭证失效：跳转 Key 设置，不清会话 */
    AIC_LLM_E_RATE    = -3,  /* 模型限流：提示等待，禁止高频重试 */
    AIC_LLM_E_PARSE   = -4,  /* 响应解析失败：保留原始响应到诊断日志 */
    AIC_LLM_E_CANCEL  = -5,  /* 用户停止 */
    AIC_LLM_E_SERVER  = -6,  /* 服务端 5xx */
    AIC_LLM_E_BADREQ  = -7,  /* 请求参数被拒 */
    AIC_LLM_E_NOMODEL = -8,  /* 模型不存在 */
    AIC_LLM_E_BALANCE = -9   /* 余额不足 */
} aic_llm_err;

aic_llm_err aic_llm_classify_http(int http_status);
const char *aic_llm_err_text(aic_llm_err e);

typedef struct aic_llm_retry_policy {
    int       max_retries;   /* 0 表示不重试 */
    int       base_delay_ms; /* 指数退避基数 */
    int       max_delay_ms;  /* 单次等待上限 */
} aic_llm_retry_policy;

aic_llm_retry_policy aic_llm_retry_policy_default(void);
/* 返回 1 表示可以重试，并写入本次等待毫秒数；0 表示不该再重试。 */
int aic_llm_retry_delay(const aic_llm_retry_policy *p, aic_llm_err e, int attempt,
                        int *out_delay_ms);

/* ---------- 请求组装（提示词 + 上下文） ---------- */
typedef struct aic_llm_request_opts {
    int    stream;            /* 默认 1 */
    double temperature;       /* <0 表示不写该字段 */
    int    max_tokens;        /* <=0 表示不写该字段 */
    int    context_messages;  /* 带入的历史消息条数 */
    /* 扩展点 message.transform 的结果：只替换「最后一条 user 消息」送进模型的文本，
     * 库里保存的仍是用户原文；NULL 表示不替换。 */
    const char *last_user_text;
    /* 扩展点 tool.provider 的结果：原样作为请求的 "tools" 数组；NULL 表示不带工具。 */
    const char *tools_json;
} aic_llm_request_opts;

aic_llm_request_opts aic_llm_request_defaults(void);

/* 组装 /chat/completions 请求体：
 * messages = [ {role:system, content:系统提示词} ] + 最近 context_messages 条未撤回消息。
 * 系统提示词由 prompt_build 生成（调用方传入），用户新消息已由调用方 append 进库。 */
int aic_llm_build_request(aic_db *db, const char *conv_id, const char *model_id,
                          const char *system_prompt, const aic_llm_request_opts *opts,
                          sb *out, char *err, size_t errlen);

/* ---------- 流式分片解析（SSE） ---------- */
typedef enum aic_llm_event_kind {
    AIC_LLM_EV_DELTA = 0,  /* 增量文本 */
    AIC_LLM_EV_USAGE = 1,  /* 用量统计 */
    AIC_LLM_EV_DONE = 2,   /* 流结束 */
    AIC_LLM_EV_ERROR = 3   /* 流内错误 */
} aic_llm_event_kind;

typedef struct aic_llm_event {
    aic_llm_event_kind kind;
    const char *text;         /* DELTA 时的增量文本（指向 parser 内部缓冲，回调内有效） */
    long long   prompt_tokens;
    long long   completion_tokens;
    aic_llm_err err;
    char        message[256];
} aic_llm_event;

typedef void (*aic_llm_event_fn)(void *ud, const aic_llm_event *ev);

typedef struct aic_sse_parser {
    sb   buf;      /* 未处理完的行缓冲 */
    sb   raw;      /* 原始响应（解析失败时进诊断日志） */
    int  saw_done;
    int  finished;
    size_t delta_count;
} aic_sse_parser;

void aic_sse_init(aic_sse_parser *p);
void aic_sse_free(aic_sse_parser *p);
/* 送入任意切分的字节片段；事件通过回调同步派发。 */
int aic_sse_feed(aic_sse_parser *p, const char *data, size_t len,
                 aic_llm_event_fn fn, void *ud);
/* 流结束收尾：缓冲里剩余的不完整行按同规则处理一次。 */
int aic_sse_finish(aic_sse_parser *p, aic_llm_event_fn fn, void *ud);

/* ---------- 传输层注入（默认不可用） ---------- */
typedef struct aic_llm_transport {
    void *ud;
    /* 发起流式请求；返回 AIC_LLM_OK 表示流已正常走完。
     * cancel 指向引擎的停止标志：传输实现应周期性检查，置 1 时中止并返回 AIC_LLM_E_CANCEL。 */
    aic_llm_err (*post_stream)(void *ud, const char *url, const char *api_key,
                               const char *body, aic_sse_parser *parser,
                               aic_llm_event_fn fn, void *cb_ud,
                               volatile int *cancel, char *err, size_t errlen);
    /* 连通性校验（新手指南第 2 步用它）。 */
    aic_llm_err (*probe)(void *ud, const char *url, const char *api_key,
                         char *err, size_t errlen);
} aic_llm_transport;

/* 默认传输：明确返回"不可用"，绝不假装成功。 */
aic_llm_err aic_llm_transport_unavailable_post(void *ud, const char *url, const char *api_key,
                                               const char *body, aic_sse_parser *parser,
                                               aic_llm_event_fn fn, void *cb_ud,
                                               volatile int *cancel, char *err, size_t errlen);
aic_llm_err aic_llm_transport_unavailable_probe(void *ud, const char *url, const char *api_key,
                                                char *err, size_t errlen);
aic_llm_transport aic_llm_transport_default(void);

#ifdef _WIN32
/* Windows 侧的真实传输（WinHTTP 流式，见 src/platform/win/http_win.c）。
 * 与 aic_llm_transport_default 不同：它真的会发请求，需要网络与有效 Key。 */
aic_llm_transport aic_win_http_transport(void);
#endif

/* ---------- 余额（FR-11） ---------- */
typedef struct aic_llm_balance {
    int       valid;          /* 0 表示本次查询失败，下面字段是上次结果 */
    int64_t   available_micros;
    int64_t   pending_micros; /* 未出账金额 */
    int64_t   local_used_micros; /* 本地账本累计 */
    int64_t   queried_ms;     /* 结果时间 */
    char      source[32];     /* "账户接口" / "本地账本" / "上次结果" */
    int       stale;          /* 1 表示是过期结果 */
    int       below_alert;    /* 1 表示低于告警阈值 */
} aic_llm_balance;

/* 解析账户余额接口响应：total_balance / granted_balance / topped_up_balance 等字段名兼容。
 * 失败返回错误码，调用方保留上次结果。 */
int aic_llm_balance_parse(const char *json, size_t len, aic_llm_balance *out,
                          char *err, size_t errlen);
/* 余额查询失败时用上次结果填充（stale=1）。 */
void aic_llm_balance_keep_last(aic_llm_balance *out, const aic_llm_balance *last, int64_t now_ms);
/* 低于阈值判断。 */
int aic_llm_balance_below(const aic_llm_balance *b, int64_t threshold_micros);

#ifdef __cplusplus
}
#endif

#endif /* AIC_LLM_H */
