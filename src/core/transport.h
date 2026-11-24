/* transport.h —— 传输适配层（技术设计 §4.1 / §16.1，FR-25）。
 *
 * 两条通道：
 *  - DSC_CH_LAN：同一局域网内用 UDP 广播做服务发现、TCP 直连传业务帧；
 *  - DSC_CH_BLUETOOTH：蓝牙 RFCOMM 串行通道（Windows 走 AF_BTH 的 RFCOMM 套接字，
 *    Android 走平台的 BluetoothSocket，由 Java 侧建立后把字节流转交 C 侧）。
 *    蓝牙通道不做 UDP 发现，设备地址就是蓝牙地址或已配对的点名，port 是 RFCOMM 通道号。
 *
 * 统一抽象 discover / connect / send / receive / close，上层同步逻辑不区分介质。
 * 帧格式：4 字节大端长度 + 载荷，避免 TCP 粘包。 */
#ifndef DSC_TRANSPORT_H
#define DSC_TRANSPORT_H

#include "dsc_types.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_TRANSPORT_DEFAULT_PORT 45871
#define DSC_TRANSPORT_MAX_FRAME    (1u << 20)
#define DSC_TRANSPORT_ADDR_MAX     64
#define DSC_TRANSPORT_PEER_MAX     16
#define DSC_DISCOVER_MAGIC         "SAI-DISCOVER 1"
#define DSC_DISCOVER_REPLY         "SAI-HERE"

#define DSC_BT_CHANNEL_MIN       1
#define DSC_BT_CHANNEL_MAX       30
#define DSC_BT_ADDR_MAX          32
#define DSC_TRANSPORT_RING_CAP   (512u * 1024u) /* Android 蓝牙字节流中转缓冲 */

typedef enum dsc_channel {
    DSC_CH_LAN = 0,
    DSC_CH_BLUETOOTH = 1,
    DSC_CH_COUNT
} dsc_channel;

typedef enum dsc_transport_state {
    DSC_TRANSPORT_CLOSED = 0,
    DSC_TRANSPORT_LISTENING,
    DSC_TRANSPORT_CONNECTED
} dsc_transport_state;

typedef struct dsc_peer {
    char        device_id[40];
    char        name[64];
    char        addr[DSC_TRANSPORT_ADDR_MAX];
    int         port;
    dsc_channel channel;
} dsc_peer;

typedef struct dsc_transport dsc_transport;

const char        *dsc_channel_name(dsc_channel ch);
int                dsc_transport_available(dsc_channel ch);
int                dsc_transport_create(dsc_channel ch, dsc_transport **out);
void               dsc_transport_destroy(dsc_transport *t);
dsc_transport_state dsc_transport_get_state(const dsc_transport *t);
void               dsc_transport_close(dsc_transport *t);

/* 监听本端端口（连接被连接方用）。 */
int dsc_transport_listen(dsc_transport *t, int port, char *err, size_t errlen);
/* 等待对端连入，返回已建立的连接。 */
int dsc_transport_accept(dsc_transport *t, int timeout_ms, char *err, size_t errlen);
/* 主动连接。 */
int dsc_transport_connect(dsc_transport *t, const char *host, int port, int timeout_ms,
                          char *err, size_t errlen);

/* 发送一帧（内部加长度前缀）。 */
int dsc_transport_send(dsc_transport *t, const void *data, size_t len, char *err, size_t errlen);
/* 收一帧（阻塞至多 timeout_ms，返回堆分配的 payload，调用方 dsc_xfree）。 */
int dsc_transport_recv(dsc_transport *t, char **out, size_t *out_len, int timeout_ms,
                       char *err, size_t errlen);

/* 局域网发现：广播探测并收集应答，最多等 wait_ms。 */
int dsc_transport_discover(int port, int wait_ms, dsc_peer *out, size_t cap, size_t *out_n,
                           char *err, size_t errlen);
/* 作为被连接方应答发现广播（后台线程调用方式由上层决定，这里只做一轮）。 */
int dsc_transport_respond(int port, int wait_ms, const char *device_id, const char *name,
                          int *out_answered, char *err, size_t errlen);

/* 本机 IPv4 地址（局域网地址，用于生成配对提示与直连）；失败返回空串。 */
int dsc_transport_local_ip(char *out, size_t cap);

/* 选择配对要用的通道：DSC_CH_LAN / DSC_CH_BLUETOOTH；-1 表示用默认
 * （局域网可用就用局域网，否则蓝牙）。之后 host / connect / discover 都走这条通道。
 * 已经建立链路时拒绝切换（返回 DSC_E_STATE）。 */
struct dsc_session;
int dsc_session_use_channel(struct dsc_session *s, int channel);

/* 蓝牙通道：列出已配对或已发现的蓝牙设备（Windows 走 BluetoothFindFirstDevice，
 * Android 走 Java 侧的回调），addr 可直接喂给 dsc_transport_connect。 */
int dsc_transport_bluetooth_peers(dsc_peer *out, size_t cap, size_t *out_n,
                                  char *err, size_t errlen);

#ifdef __ANDROID__
/* Android 蓝牙只能由 Java 的 BluetoothSocket 建立，C 侧用一个环形缓冲接字节流。
 * 连接方向由 dsc_transport_connect / listen+accept 决定：连接方调用 connect 后即使
 * 套接字稍后才到，recv 也会等它出现。 */
/* 0 = 未连接 / 1 = 已就绪；连接方 connect 返回后由 Java 把套接字登记进来。 */
int  dsc_bt_attach_fd(int fd, char *err, size_t errlen);
/* 对端主动连入的套接字（listen + accept 用）。 */
int  dsc_bt_attach_listen_fd(int fd, char *err, size_t errlen);
int  dsc_bt_attach_incoming_fd(int fd, char *err, size_t errlen);
/* 由 Java 把系统已配对/已发现的蓝牙设备清单交给 C 侧，供「列出已配对设备」显示。 */
int  dsc_bt_peers_set(const dsc_peer *list, size_t n);
void dsc_bt_detach(void);
int  dsc_bt_attached(void);
/* 把 Java 读到的字节塞进接收环形缓冲（返回空闲空间，便于背压）。 */
int  dsc_bt_feed(const void *data, size_t len);
/* 取出 C 侧要发出的字节交给 Java 写（返回拷贝的字节数）。 */
int  dsc_bt_drain(void *buf, size_t cap, size_t *out_n);
/* 查询 Java 侧写队列里还有多少字节未发出。 */
int  dsc_bt_pending_out(void);
#else
/* Windows：蓝牙 RFCOMM 地址换算（BT 地址形如 "AA:BB:CC:DD:EE:FF"，也接受
 * "aabbccddeeff" 或带分隔的十位十六进制）与是否已打开蓝牙无线电。
 * 这两项只做地址格式与无线电状态的判断，不需要蓝牙硬件即可验证。 */
int  dsc_bt_parse_addr(const char *text, unsigned long long *out_addr);
int  dsc_bt_radio_ready(char *err, size_t errlen);
/* 蓝牙通道的帧收发走独立的 transport 对象（与局域网同构），connect 的 port 即 RFCOMM 通道号。 */
#endif

#ifdef __cplusplus
}
#endif

#endif
