/* sync_session.h —— 把传输适配、配对与同步引擎串成一条可驱动的会话（§16.1 至 §16.6）。
 *
 * 用法：
 *   被连接方：aic_session_create → aic_session_host(端口, 本端码) → 循环 answer_discovery +
 *             aic_session_accept(超时) → aic_session_handshake_accept(用户输入的对端码) → 循环 pump
 *   连接方：  aic_session_create → aic_session_discover → aic_session_connect →
 *             aic_session_handshake_connect(用户输入的对端码) → 循环 pump
 *
 * pump 只做一轮，既能放进界面定时器，也能跑在后台线程里。 */
#ifndef AIC_SYNC_SESSION_H
#define AIC_SYNC_SESSION_H

#include "aic_types.h"
#include "db.h"
#include "pairing.h"
#include "sync.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_SESSION_PENDING_MAX 8

typedef struct aic_session aic_session;

int  aic_session_create(aic_db *db, aic_session **out);
void aic_session_destroy(aic_session *s);

aic_sync_state aic_session_phase(const aic_session *s);
int            aic_session_is_paired(const aic_session *s);
/* 本端配对码（未生成时为空串）。 */
const char    *aic_session_our_code(const aic_session *s);

/* 被连接方：监听端口并生成本端配对码（avoid_code 可为 NULL）。 */
int aic_session_host(aic_session *s, int port, const char *avoid_code, char *err, size_t errlen);
/* 被连接方：应答一轮局域网发现广播（放在界面定时器里反复调用）。 */
int aic_session_answer_discovery(aic_session *s, int wait_ms, int *out_answered,
                                 char *err, size_t errlen);
/* 连接方：搜索同一局域网内的对端。 */
int aic_session_discover(aic_session *s, int port, int wait_ms, aic_peer *peers, size_t cap,
                         size_t *out_n, char *err, size_t errlen);
/* 连接方：连接指定地址。 */
int aic_session_connect(aic_session *s, const char *addr, int port, int timeout_ms,
                        const char *avoid_code, char *err, size_t errlen);
/* 被连接方：等待连入。 */
int aic_session_accept(aic_session *s, int timeout_ms, char *err, size_t errlen);

/* 连接方握手：peer_code 是用户从对端屏幕读到的 6 位码。 */
int aic_session_handshake_connect(aic_session *s, const char *peer_code, int timeout_ms,
                                  char *err, size_t errlen);
/* 被连接方握手：peer_code 是用户从对端屏幕读到的 6 位码。 */
int aic_session_handshake_accept(aic_session *s, const char *peer_code, int timeout_ms,
                                 char *err, size_t errlen);

/* 一轮同步：先发发件箱，再收对端事件并按 §16.6 合并。 */
int aic_session_pump(aic_session *s, int timeout_ms, int *out_sent, int *out_applied,
                     char *err, size_t errlen);

/* 采集实体快照并入队（实时同步开启时下一轮 pump 就会发出）。 */
int aic_session_publish(aic_session *s, aic_sync_entity entity, aic_sync_op op,
                        const char *entity_id, char *err, size_t errlen);

/* 解除配对并关闭链路（两端本地数据都保留）。 */
int aic_session_unpair(aic_session *s, char *err, size_t errlen);
void aic_session_close(aic_session *s);

#ifdef __cplusplus
}
#endif

#endif
