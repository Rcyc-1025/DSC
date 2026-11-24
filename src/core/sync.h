/* sync.h —— 同步引擎（技术设计第 16 章，FR-27 / FR-28 / FR-29）。
 *
 * 设计要点：
 *  - 状态机：IDLE → DISCOVERING → CONNECTING → PAIRING → PAIRED → SYNCING；
 *    SYNCING 与 PAIRED 之间随链路断开切换，链路断开期间本地照常可写、事件入队；
 *  - 只传增量事件，事件字段 event_id / entity / op / payload / logical_ts / origin_device / schema_version；
 *  - 接收端按 event_id 去重；冲突按 (logical_ts, origin_device) 比较，落败方原始实体存 sync_conflict；
 *  - 实时同步默认开启（sync.realtime_enabled 初始 1），暂停只停新事件发送，已入队事件恢复后补发。 */
#ifndef DSC_SYNC_H
#define DSC_SYNC_H

#include "dsc_types.h"
#include "db.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_SET_SYNC_REALTIME  "sync.realtime_enabled"
#define DSC_SYNC_SCHEMA_VERSION 1
#define DSC_SYNC_ENTITY_MAX     24
#define DSC_SYNC_OP_MAX         24
#define DSC_SYNC_ID_MAX         40
#define DSC_SYNC_ENTITY_ID_MAX  64
#define DSC_SYNC_DEVICE_MAX     40

typedef enum dsc_sync_state {
    DSC_SYNC_IDLE = 0,
    DSC_SYNC_DISCOVERING,
    DSC_SYNC_CONNECTING,
    DSC_SYNC_PAIRING,
    DSC_SYNC_PAIRED,
    DSC_SYNC_SYNCING
} dsc_sync_state;

/* 参与同步的实体（§16.4）。插件二进制包、凭证、用量流水、日志、备份包不参与同步。 */
typedef enum dsc_sync_entity {
    DSC_ENT_PROFILE = 0,
    DSC_ENT_PERSONA,
    DSC_ENT_PROMPT_VERSION,
    DSC_ENT_CONVERSATION,
    DSC_ENT_MESSAGE,
    DSC_ENT_SETTING,
    DSC_ENT_PLUGIN,
    /* FR-36：AI 记忆重抽事件（memory_replace）。记忆是消息派生物，不在 §16.4 的
     * 七类实体白名单里；事件只带 source_message_id 与改后的正文，对端按同一规则
     * 重抽，不传记忆条目本身。 */
    DSC_ENT_MEMORY,
    DSC_ENT_COUNT
} dsc_sync_entity;

typedef enum dsc_sync_op {
    DSC_OP_CREATE = 0,
    DSC_OP_UPDATE,
    DSC_OP_RECALL,
    DSC_OP_EDIT,
    DSC_OP_DELETE,
    DSC_OP_COUNT
} dsc_sync_op;

typedef struct dsc_sync_event {
    char      event_id[DSC_SYNC_ID_MAX];
    char      entity[DSC_SYNC_ENTITY_MAX];
    char      op[DSC_SYNC_OP_MAX];
    char      entity_id[DSC_SYNC_ENTITY_ID_MAX];
    char     *payload; /* 实体快照 JSON（堆分配，用 dsc_sync_event_free 释放） */
    long long logical_ts;
    char      origin_device[DSC_SYNC_DEVICE_MAX];
    int       schema_version;
} dsc_sync_event;

typedef struct dsc_sync dsc_sync;

int  dsc_sync_create(dsc_db *db, dsc_sync **out);
void dsc_sync_destroy(dsc_sync *s);

dsc_sync_state dsc_sync_get_state(const dsc_sync *s);
const char    *dsc_sync_state_name(dsc_sync_state st);
/* 按 §16.3 校验迁移合法性；不合法返回 DSC_E_STATE 并写 err。 */
int dsc_sync_enter(dsc_sync *s, dsc_sync_state next, char *err, size_t errlen);

const char *dsc_sync_entity_name(dsc_sync_entity e);
int         dsc_sync_entity_parse(const char *name);
const char *dsc_sync_op_name(dsc_sync_op op);
int         dsc_sync_op_parse(const char *name);

/* 实时同步开关：配对成功后默认开启；暂停留 0。 */
int dsc_sync_realtime_enabled(dsc_db *db);
int dsc_sync_set_realtime(dsc_db *db, int enabled);

/* 发布事件：取逻辑时钟、写入发件箱、返回 event_id。payload 必须是 JSON 对象文本。 */
int dsc_sync_publish(dsc_sync *s, dsc_sync_entity entity, dsc_sync_op op,
                     const char *entity_id, const char *payload,
                     char *out_event_id, size_t cap, char *err, size_t errlen);

/* 直接从库里取实体快照并发布（调用方只需给 id；profile 可给 NULL）。 */
int dsc_sync_capture(dsc_sync *s, dsc_sync_entity entity, dsc_sync_op op,
                     const char *entity_id, char *out_event_id, size_t cap,
                     char *err, size_t errlen);

int  dsc_sync_outbox_count(dsc_db *db, int *out_count);
int  dsc_sync_outbox_next(dsc_sync *s, dsc_sync_event *out);
int  dsc_sync_outbox_mark_sent(dsc_sync *s, const char *event_id);
void dsc_sync_event_free(dsc_sync_event *ev);

int dsc_sync_event_to_json(const dsc_sync_event *ev, sb *out);
int dsc_sync_event_from_json(const char *text, size_t len, dsc_sync_event *ev,
                             char *err, size_t errlen);

/* 接收端应用事件：去重、冲突合并、更新游标。重复事件返回 DSC_OK 且 out_applied=0。 */
int dsc_sync_apply_event(dsc_sync *s, const dsc_sync_event *ev, int *out_applied,
                         char *err, size_t errlen);
int dsc_sync_apply_json(dsc_sync *s, const char *text, size_t len, int *out_applied,
                        char *err, size_t errlen);

/* 冲突副本与游标查询（界面展示用）。 */
int dsc_sync_conflict_count(dsc_db *db, int *out_count);
int dsc_sync_conflicts_json(dsc_db *db, sb *out);
int dsc_sync_cursor_get(dsc_db *db, const char *peer_id, char *out_event, size_t cap,
                        long long *out_ts);

#ifdef __cplusplus
}
#endif

#endif
