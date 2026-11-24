/* sync_session.h —— 把传输适配、配对与同步引擎串成一条可驱动的会话（§16.1 至 §16.6）。
 *
 * 用法：
 *   被连接方：dsc_session_create → dsc_session_host(端口, 本端码) → 循环 answer_discovery +
 *             dsc_session_accept(超时) → dsc_session_handshake_accept(用户输入的对端码) → 循环 pump
 *   连接方：  dsc_session_create → dsc_session_discover → dsc_session_connect →
 *             dsc_session_handshake_connect(用户输入的对端码) → 循环 pump
 *
 * pump 只做一轮，既能放进界面定时器，也能跑在后台线程里。 */
#ifndef DSC_SYNC_SESSION_H
#define DSC_SYNC_SESSION_H

#include "dsc_types.h"
#include "db.h"
#include "pairing.h"
#include "sync.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_SESSION_PENDING_MAX 8

typedef struct dsc_session dsc_session;

int  dsc_session_create(dsc_db *db, dsc_session **out);
void dsc_session_destroy(dsc_session *s);

dsc_sync_state dsc_session_phase(const dsc_session *s);
int            dsc_session_is_paired(const dsc_session *s);
/* 本端配对码（未生成时为空串）。 */
const char    *dsc_session_our_code(const dsc_session *s);

/* 被连接方：监听端口并生成本端配对码（avoid_code 可为 NULL）。 */
int dsc_session_host(dsc_session *s, int port, const char *avoid_code, char *err, size_t errlen);
/* 被连接方：应答一轮局域网发现广播（放在界面定时器里反复调用）。 */
int dsc_session_answer_discovery(dsc_session *s, int wait_ms, int *out_answered,
                                 char *err, size_t errlen);
/* 连接方：搜索同一局域网内的对端。 */
int dsc_session_discover(dsc_session *s, int port, int wait_ms, dsc_peer *peers, size_t cap,
                         size_t *out_n, char *err, size_t errlen);
/* 连接方：连接指定地址。 */
int dsc_session_connect(dsc_session *s, const char *addr, int port, int timeout_ms,
                        const char *avoid_code, char *err, size_t errlen);
/* 被连接方：等待连入。 */
int dsc_session_accept(dsc_session *s, int timeout_ms, char *err, size_t errlen);

/* 连接方握手：peer_code 是用户从对端屏幕读到的 6 位码。 */
int dsc_session_handshake_connect(dsc_session *s, const char *peer_code, int timeout_ms,
                                  char *err, size_t errlen);
/* 被连接方握手：peer_code 是用户从对端屏幕读到的 6 位码。 */
int dsc_session_handshake_accept(dsc_session *s, const char *peer_code, int timeout_ms,
                                 char *err, size_t errlen);

/* 一轮同步：先发发件箱，再收对端事件并按 §16.6 合并。 */
int dsc_session_pump(dsc_session *s, int timeout_ms, int *out_sent, int *out_applied,
                     char *err, size_t errlen);

/* 采集实体快照并入队（实时同步开启时下一轮 pump 就会发出）。 */
int dsc_session_publish(dsc_session *s, dsc_sync_entity entity, dsc_sync_op op,
                        const char *entity_id, char *err, size_t errlen);

/* 解除配对并关闭链路（两端本地数据都保留）。 */
int dsc_session_unpair(dsc_session *s, char *err, size_t errlen);
void dsc_session_close(dsc_session *s);

#ifdef __cplusplus
}
#endif

#endif
