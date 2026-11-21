/* engine.h —— 对话引擎：把「用户输入 → 提示词 → 请求 → 流式回复 → 落库 → 用量」串起来。
 *
 * 传输层由外部注入（见 llm.h 的 aic_llm_transport）：默认不可用，测试里用一个假传输喂 SSE 片段，
 * 因此整条链路（含用量记账与费用）都可以在没有网络的情况下验证。 */
#ifndef AIC_ENGINE_H
#define AIC_ENGINE_H

#include <stddef.h>

#include "aic_types.h"
#include "db.h"
#include "llm.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aic_engine_result {
    char  user_msg_id[40];
    char  assistant_msg_id[40];
    char *text;                /* 助手最终文本（堆分配，用 aic_engine_result_free 释放） */
    long long prompt_tokens;
    long long completion_tokens;
    long long cost_micros;
    int       retries;
    aic_llm_err llm_err;       /* AIC_LLM_OK 表示成功 */
} aic_engine_result;

typedef struct aic_engine aic_engine;

int  aic_engine_create(aic_db *db, aic_engine **out);
void aic_engine_destroy(aic_engine *e);

void aic_engine_set_transport(aic_engine *e, aic_llm_transport tr);
void aic_engine_set_endpoint(aic_engine *e, const char *url);
/* API Key 只保存在内存里用于组装请求头；日志与导出永远不写它。 */
void aic_engine_set_api_key(aic_engine *e, const char *key);
int  aic_engine_has_key(const aic_engine *e);
void aic_engine_set_context_messages(aic_engine *e, int n);
/* 插件注入片段（按扩展点声明顺序拼好后传入）。 */
void aic_engine_set_plugin_fragment(aic_engine *e, const char *fragment);
/* 会话级模型覆盖（只影响后续请求，不改已存消息）。 */
void aic_engine_set_model_override(aic_engine *e, const char *model_id);

/* 组装一次请求（调试面板用，不发送）。 */
int aic_engine_build_request(aic_engine *e, const char *conv_id, sb *out, char *err, size_t errlen);

/* 发送一条用户消息并等待流式回复。
 * 失败语义：用户消息保留（草稿不丢），不写入空的助手消息；llm_err 给出分类。 */
int aic_engine_send(aic_engine *e, const char *conv_id, const char *user_text,
                    aic_engine_result *res, char *err, size_t errlen);

/* 请求停止（在回调里被置位的取消标志）。 */
void aic_engine_stop(aic_engine *e);
int  aic_engine_is_stopped(const aic_engine *e);

/* 撤回时间窗：默认 120 秒，可由设置调整（FR-10）。 */
int aic_engine_can_recall(aic_engine *e, const char *msg_id, int window_seconds);
int aic_engine_recall(aic_engine *e, const char *msg_id, int window_seconds,
                      char *err, size_t errlen);
/* 修改消息：超窗也能改，但首次修改会保留原文。 */
int aic_engine_edit(aic_engine *e, const char *msg_id, const char *new_content,
                    char *err, size_t errlen);

/* 会话切换时重建上下文统计（FR-13：切换不串台）。 */
int aic_engine_context_stats(aic_engine *e, const char *conv_id, int *out_total,
                             int *out_used, long long *out_tokens_estimate);

void aic_engine_result_free(aic_engine_result *r);

#ifdef __cplusplus
}
#endif

#endif /* AIC_ENGINE_H */
