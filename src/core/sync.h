/* sync.h —— 同步引擎（技术设计第 16 章，FR-27 / FR-28 / FR-29）。
 *
 * 设计要点：
 *  - 状态机：IDLE → DISCOVERING → CONNECTING → PAIRING → PAIRED → SYNCING；
 *    SYNCING 与 PAIRED 之间随链路断开切换，链路断开期间本地照常可写、事件入队；
 *  - 只传增量事件，事件字段 event_id / entity / op / payload / logical_ts / origin_device / schema_version；
 *  - 接收端按 event_id 去重；冲突按 (logical_ts, origin_device) 比较，落败方原始实体存 sync_conflict；
 *  - 实时同步默认开启（sync.realtime_enabled 初始 1），暂停只停新事件发送，已入队事件恢复后补发。 */
#ifndef AIC_SYNC_H
#define AIC_SYNC_H

#include "aic_types.h"
#include "db.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_SET_SYNC_REALTIME  "sync.realtime_enabled"
#define AIC_SYNC_SCHEMA_VERSION 1
#define AIC_SYNC_ENTITY_MAX     24
#define AIC_SYNC_OP_MAX         24
#define AIC_SYNC_ID_MAX         40
#define AIC_SYNC_ENTITY_ID_MAX  64
#define AIC_SYNC_DEVICE_MAX     40

typedef enum aic_sync_state {
    AIC_SYNC_IDLE = 0,
    AIC_SYNC_DISCOVERING,
    AIC_SYNC_CONNECTING,
    AIC_SYNC_PAIRING,
    AIC_SYNC_PAIRED,
    AIC_SYNC_SYNCING
} aic_sync_state;

/* 参与同步的实体（§16.4）。插件二进制包、凭证、用量流水、日志、备份包不参与同步。 */
typedef enum aic_sync_entity {
    AIC_ENT_PROFILE = 0,
    AIC_ENT_PERSONA,
    AIC_ENT_PROMPT_VERSION,
    AIC_ENT_CONVERSATION,
    AIC_ENT_MESSAGE,
    AIC_ENT_SETTING,
    AIC_ENT_PLUGIN,
    AIC_ENT_COUNT
} aic_sync_entity;

typedef enum aic_sync_op {
    AIC_OP_CREATE = 0,
    AIC_OP_UPDATE,
    AIC_OP_RECALL,
    AIC_OP_EDIT,
    AIC_OP_DELETE,
    AIC_OP_COUNT
} aic_sync_op;

typedef struct aic_sync_event {
    char      event_id[AIC_SYNC_ID_MAX];
    char      entity[AIC_SYNC_ENTITY_MAX];
    char      op[AIC_SYNC_OP_MAX];
    char      entity_id[AIC_SYNC_ENTITY_ID_MAX];
    char     *payload; /* 实体快照 JSON（堆分配，用 aic_sync_event_free 释放） */
    long long logical_ts;
    char      origin_device[AIC_SYNC_DEVICE_MAX];
    int       schema_version;
} aic_sync_event;

typedef struct aic_sync aic_sync;

int  aic_sync_create(aic_db *db, aic_sync **out);
void aic_sync_destroy(aic_sync *s);

aic_sync_state aic_sync_get_state(const aic_sync *s);
const char    *aic_sync_state_name(aic_sync_state st);
/* 按 §16.3 校验迁移合法性；不合法返回 AIC_E_STATE 并写 err。 */
int aic_sync_enter(aic_sync *s, aic_sync_state next, char *err, size_t errlen);

const char *aic_sync_entity_name(aic_sync_entity e);
int         aic_sync_entity_parse(const char *name);
const char *aic_sync_op_name(aic_sync_op op);
int         aic_sync_op_parse(const char *name);

/* 实时同步开关：配对成功后默认开启；暂停留 0。 */
int aic_sync_realtime_enabled(aic_db *db);
int aic_sync_set_realtime(aic_db *db, int enabled);

/* 发布事件：取逻辑时钟、写入发件箱、返回 event_id。payload 必须是 JSON 对象文本。 */
int aic_sync_publish(aic_sync *s, aic_sync_entity entity, aic_sync_op op,
                     const char *entity_id, const char *payload,
                     char *out_event_id, size_t cap, char *err, size_t errlen);

/* 直接从库里取实体快照并发布（调用方只需给 id；profile 可给 NULL）。 */
int aic_sync_capture(aic_sync *s, aic_sync_entity entity, aic_sync_op op,
                     const char *entity_id, char *out_event_id, size_t cap,
                     char *err, size_t errlen);

int  aic_sync_outbox_count(aic_db *db, int *out_count);
int  aic_sync_outbox_next(aic_sync *s, aic_sync_event *out);
int  aic_sync_outbox_mark_sent(aic_sync *s, const char *event_id);
void aic_sync_event_free(aic_sync_event *ev);

int aic_sync_event_to_json(const aic_sync_event *ev, sb *out);
int aic_sync_event_from_json(const char *text, size_t len, aic_sync_event *ev,
                             char *err, size_t errlen);

/* 接收端应用事件：去重、冲突合并、更新游标。重复事件返回 AIC_OK 且 out_applied=0。 */
int aic_sync_apply_event(aic_sync *s, const aic_sync_event *ev, int *out_applied,
                         char *err, size_t errlen);
int aic_sync_apply_json(aic_sync *s, const char *text, size_t len, int *out_applied,
                        char *err, size_t errlen);

/* 冲突副本与游标查询（界面展示用）。 */
int aic_sync_conflict_count(aic_db *db, int *out_count);
int aic_sync_conflicts_json(aic_db *db, sb *out);
int aic_sync_cursor_get(aic_db *db, const char *peer_id, char *out_event, size_t cap,
                        long long *out_ts);

#ifdef __cplusplus
}
#endif

#endif
