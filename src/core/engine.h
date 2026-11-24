/* engine.h —— 对话引擎：把「用户输入 → 提示词 → 请求 → 流式回复 → 落库 → 用量」串起来。
 *
 * 传输层由外部注入（见 llm.h 的 dsc_llm_transport）：默认不可用，测试里用一个假传输喂 SSE 片段，
 * 因此整条链路（含用量记账与费用）都可以在没有网络的情况下验证。 */
#ifndef DSC_ENGINE_H
#define DSC_ENGINE_H

#include <stddef.h>

#include "dsc_types.h"
#include "db.h"
#include "llm.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 同步会话（sync.h）；引擎只用到"发布 memory_replace 事件"这一处。 */
typedef struct dsc_sync dsc_sync;

typedef struct dsc_engine_result {
    char  user_msg_id[40];
    char  assistant_msg_id[40];
    char *text;                /* 助手最终文本（堆分配，用 dsc_engine_result_free 释放） */
    long long prompt_tokens;
    long long completion_tokens;
    long long cost_micros;
    int       retries;
    dsc_llm_err llm_err;       /* DSC_LLM_OK 表示成功 */
} dsc_engine_result;

typedef struct dsc_engine dsc_engine;

int  dsc_engine_create(dsc_db *db, dsc_engine **out);
void dsc_engine_destroy(dsc_engine *e);

void dsc_engine_set_transport(dsc_engine *e, dsc_llm_transport tr);
void dsc_engine_set_endpoint(dsc_engine *e, const char *url);
/* API Key 只保存在内存里用于组装请求头；日志与导出永远不写它。 */
void dsc_engine_set_api_key(dsc_engine *e, const char *key);
int  dsc_engine_has_key(const dsc_engine *e);
void dsc_engine_set_context_messages(dsc_engine *e, int n);
/* 插件注入片段（按扩展点声明顺序拼好后传入）。 */
void dsc_engine_set_plugin_fragment(dsc_engine *e, const char *fragment);
/* 会话级模型覆盖（只影响后续请求，不改已存消息）。 */
void dsc_engine_set_model_override(dsc_engine *e, const char *model_id);

/* 组装一次请求（调试面板用，不发送）。 */
int dsc_engine_build_request(dsc_engine *e, const char *conv_id, sb *out, char *err, size_t errlen);

/* 发送一条用户消息并等待流式回复。
 * 失败语义：用户消息保留（草稿不丢），不写入空的助手消息；llm_err 给出分类。 */
int dsc_engine_send(dsc_engine *e, const char *conv_id, const char *user_text,
                    dsc_engine_result *res, char *err, size_t errlen);

/* 流式增量回调（在调用 send 的线程上同步触发；界面据此边收边显示）。 */
void dsc_engine_set_stream_cb(dsc_engine *e, void (*fn)(void *ud, const char *delta), void *ud);

/* 扩展点挂钩（技术设计 9.1 / 10.1）：
 *   message.transform —— 改写"送进模型的最后一条 user 文本"，库里仍保存用户原文；
 *   tool.provider     —— 提供请求里的 tools 数组（返回原始 JSON 数组文本）。
 * 任一挂钩返回非 DSC_OK 时按"该扩展点不可用"处理，不影响正常对话。 */
typedef int (*dsc_engine_text_fn)(void *ud, const char *text, char *out, size_t cap,
                                  char *err, size_t errlen);
typedef int (*dsc_engine_tools_fn)(void *ud, sb *out_json, char *err, size_t errlen);
void dsc_engine_set_transform(dsc_engine *e, dsc_engine_text_fn fn, void *ud);
void dsc_engine_set_tools_provider(dsc_engine *e, dsc_engine_tools_fn fn, void *ud);

/* 重新生成上一条回复（技术设计 6.1）：旧回复标记为 superseded（保留可回看），
 * 新回复获得新 id。out_old_id 可为空。 */
int  dsc_engine_regenerate(dsc_engine *e, const char *conv_id, char *out_old_id, size_t old_cap,
                           dsc_engine_result *res, char *err, size_t errlen);

/* 请求停止（在回调里被置位的取消标志）。 */
void dsc_engine_stop(dsc_engine *e);
int  dsc_engine_is_stopped(const dsc_engine *e);

/* 撤回时间窗：默认 120 秒，可由设置调整（FR-10）。 */
int dsc_engine_can_recall(dsc_engine *e, const char *msg_id, int window_seconds);
int dsc_engine_recall(dsc_engine *e, const char *msg_id, int window_seconds,
                      char *err, size_t errlen);
/* 修改消息：超窗也能改，但首次修改会保留原文。 */
int dsc_engine_edit(dsc_engine *e, const char *msg_id, const char *new_content,
                    char *err, size_t errlen);

/* ---------- FR-36：AI 发言修改与记忆同步 ---------- */
/* AI 发言修改不设时间窗；提交前界面必须提示"修改将同步更改 AI 记忆"。
 * 实现：同一事务内更新 content/original_content/edited_at、按 source_message_id 物理删除
 * 全部 memory_entry 与关键词索引后重新抽取写入，失败整体回滚（不软删除、不留副本）；
 * 提交成功后若有同步会话，发一条 memory_replace 事件让对端重抽。
 * out_removed / out_added 可为空。 */
int dsc_engine_edit_ai(dsc_engine *e, const char *msg_id, const char *new_content,
                       int *out_mem_removed, int *out_mem_added,
                       char *err, size_t errlen);

/* 数据目录：错误中心明细（errors/<id>.json）写在这里；为空时错误只入库不落明细。 */
void dsc_engine_set_data_root(dsc_engine *e, const char *path);
void dsc_engine_set_origin_device(dsc_engine *e, const char *device_id);
/* 挂同步会话，用于发 memory_replace 事件；可为空。 */
void dsc_engine_set_sync(dsc_engine *e, dsc_sync *sync);
/* 关闭后不再抽取记忆 / 不再注入设备能力（默认都开）。 */
void dsc_engine_set_memory_enabled(dsc_engine *e, int on);
void dsc_engine_set_capability_enabled(dsc_engine *e, int on);

/* 会话切换时重建上下文统计（FR-13：切换不串台）。 */
int dsc_engine_context_stats(dsc_engine *e, const char *conv_id, int *out_total,
                             int *out_used, long long *out_tokens_estimate);

void dsc_engine_result_free(dsc_engine_result *r);

#ifdef __cplusplus
}
#endif

#endif /* DSC_ENGINE_H */
