/* transport.h —— 传输适配层（技术设计 §4.1 / §16.1，FR-25）。
 *
 * 两条通道：
 *  - AIC_CH_LAN：同一局域网内用 UDP 广播做服务发现、TCP 直连传业务帧；
 *  - AIC_CH_BLUETOOTH：平台串行或低功耗通道，本实现只留接口（能力探测返回不可用）。
 *
 * 统一抽象 discover / connect / send / receive / close，上层同步逻辑不区分介质。
 * 帧格式：4 字节大端长度 + 载荷，避免 TCP 粘包。 */
#ifndef AIC_TRANSPORT_H
#define AIC_TRANSPORT_H

#include "aic_types.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_TRANSPORT_DEFAULT_PORT 45871
#define AIC_TRANSPORT_MAX_FRAME    (1u << 20)
#define AIC_TRANSPORT_ADDR_MAX     64
#define AIC_TRANSPORT_PEER_MAX     16
#define AIC_DISCOVER_MAGIC         "SAI-DISCOVER 1"
#define AIC_DISCOVER_REPLY         "SAI-HERE"

typedef enum aic_channel {
    AIC_CH_LAN = 0,
    AIC_CH_BLUETOOTH = 1,
    AIC_CH_COUNT
} aic_channel;

typedef enum aic_transport_state {
    AIC_TRANSPORT_CLOSED = 0,
    AIC_TRANSPORT_LISTENING,
    AIC_TRANSPORT_CONNECTED
} aic_transport_state;

typedef struct aic_peer {
    char        device_id[40];
    char        name[64];
    char        addr[AIC_TRANSPORT_ADDR_MAX];
    int         port;
    aic_channel channel;
} aic_peer;

typedef struct aic_transport aic_transport;

const char        *aic_channel_name(aic_channel ch);
int                aic_transport_available(aic_channel ch);
int                aic_transport_create(aic_channel ch, aic_transport **out);
void               aic_transport_destroy(aic_transport *t);
aic_transport_state aic_transport_get_state(const aic_transport *t);
void               aic_transport_close(aic_transport *t);

/* 监听本端端口（连接被连接方用）。 */
int aic_transport_listen(aic_transport *t, int port, char *err, size_t errlen);
/* 等待对端连入，返回已建立的连接。 */
int aic_transport_accept(aic_transport *t, int timeout_ms, char *err, size_t errlen);
/* 主动连接。 */
int aic_transport_connect(aic_transport *t, const char *host, int port, int timeout_ms,
                          char *err, size_t errlen);

/* 发送一帧（内部加长度前缀）。 */
int aic_transport_send(aic_transport *t, const void *data, size_t len, char *err, size_t errlen);
/* 收一帧（阻塞至多 timeout_ms，返回堆分配的 payload，调用方 aic_xfree）。 */
int aic_transport_recv(aic_transport *t, char **out, size_t *out_len, int timeout_ms,
                       char *err, size_t errlen);

/* 局域网发现：广播探测并收集应答，最多等 wait_ms。 */
int aic_transport_discover(int port, int wait_ms, aic_peer *out, size_t cap, size_t *out_n,
                           char *err, size_t errlen);
/* 作为被连接方应答发现广播（后台线程调用方式由上层决定，这里只做一轮）。 */
int aic_transport_respond(int port, int wait_ms, const char *device_id, const char *name,
                          int *out_answered, char *err, size_t errlen);

/* 本机 IPv4 地址（局域网地址，用于生成配对提示与直连）；失败返回空串。 */
int aic_transport_local_ip(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
