/* transport_android.c —— Android 传输适配层（BSD socket）：局域网 TCP 直连 + UDP 发现，
 * 以及蓝牙 RFCOMM 串行通道（技术设计 §16.1）。
 *
 * 蓝牙的套接字只能由 Java 的 BluetoothSocket 建立，C 侧不直接持有它：
 * Java 用 aicBtAttachFd / aicBtAttachListenFd / aicBtAttachIncomingFd 登记套接字与 fd，
 * 收方向用 aicBtFeed 把读到的字节塞进环形缓冲，发方向由 C 写进缓冲、Java 用 aicBtDrain 取走。
 * 帧格式与局域网一致，所以上层同步逻辑不区分介质。 */
#include "transport.h"

/* 已配对设备缓存（供 aic_transport_bluetooth_peers 读）。 */
int aic_bt_peers_cache(aic_peer *out, size_t cap, size_t *out_n);

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- 蓝牙字节流中转（Java 与 C 之间） ---------- */

static pthread_mutex_t g_bt_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bt_cond = PTHREAD_COND_INITIALIZER;
static unsigned char   g_bt_in[AIC_TRANSPORT_RING_CAP];
static size_t          g_bt_in_head = 0;
static size_t          g_bt_in_len = 0;
static int             g_bt_listen_fd = -1;
static int             g_bt_incoming_fd = -1;
static int             g_bt_conn_fd = -1;
static int             g_bt_attached = 0;
static int             g_bt_pending_out = 0; /* 已交给 Java 但尚未写出的字节数 */
#define BT_OUT_CAP (512u * 1024u)
static unsigned char   g_bt_out[BT_OUT_CAP];
static size_t          g_bt_out_len = 0;

static void bt_ring_push(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    pthread_mutex_lock(&g_bt_mtx);
    while (len > 0) {
        size_t free_at = (g_bt_in_head + g_bt_in_len) % AIC_TRANSPORT_RING_CAP;
        if (g_bt_in_len == AIC_TRANSPORT_RING_CAP) break; /* 满：丢弃多余字节，上层会因帧不完整而超时 */
        g_bt_in[free_at] = *p++;
        g_bt_in_len++;
        len--;
    }
    pthread_cond_broadcast(&g_bt_cond);
    pthread_mutex_unlock(&g_bt_mtx);
}

/* 从环形缓冲取最多 need 字节；wait_ms < 0 表示一直等到取够。
 * 返回取到的字节数（0 表示超时）。 */
static size_t bt_ring_pop(void *dst, size_t need, int wait_ms)
{
    unsigned char *out = (unsigned char *)dst;
    size_t got = 0;
    pthread_mutex_lock(&g_bt_mtx);
    struct timespec ts;
    if (wait_ms >= 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait_ms / 1000;
        ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    }
    while (got < need) {
        if (g_bt_in_len > 0) {
            size_t take = need - got;
            if (take > g_bt_in_len) take = g_bt_in_len;
            size_t first = AIC_TRANSPORT_RING_CAP - g_bt_in_head;
            if (first > take) first = take;
            memcpy(out + got, g_bt_in + g_bt_in_head, first);
            if (take > first) memcpy(out + got + first, g_bt_in, take - first);
            g_bt_in_head = (g_bt_in_head + take) % AIC_TRANSPORT_RING_CAP;
            g_bt_in_len -= take;
            got += take;
            continue;
        }
        if (wait_ms < 0 || !g_bt_attached) break;
        int rc = pthread_cond_timedwait(&g_bt_cond, &g_bt_mtx, &ts);
        if (rc != 0) break;
    }
    pthread_mutex_unlock(&g_bt_mtx);
    return got;
}

/* Java 侧登记的已配对设备清单（见 aic_bt_peers_set）。 */
int aic_bt_peers_set(const aic_peer *list, size_t n);

int aic_bt_attach_fd(int fd, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (fd < 0) {
        if (err && errlen) snprintf(err, errlen, "无效的蓝牙套接字");
        return AIC_E_INVAL;
    }
    pthread_mutex_lock(&g_bt_mtx);
    g_bt_conn_fd = fd;
    g_bt_attached = 1;
    pthread_cond_broadcast(&g_bt_cond);
    pthread_mutex_unlock(&g_bt_mtx);
    return AIC_OK;
}

int aic_bt_attach_listen_fd(int fd, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (fd < 0) {
        if (err && errlen) snprintf(err, errlen, "无效的蓝牙监听套接字");
        return AIC_E_INVAL;
    }
    pthread_mutex_lock(&g_bt_mtx);
    g_bt_listen_fd = fd;
    pthread_mutex_unlock(&g_bt_mtx);
    return AIC_OK;
}

int aic_bt_attach_incoming_fd(int fd, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (fd < 0) {
        if (err && errlen) snprintf(err, errlen, "无效的蓝牙连接套接字");
        return AIC_E_INVAL;
    }
    pthread_mutex_lock(&g_bt_mtx);
    g_bt_incoming_fd = fd;
    g_bt_attached = 1;
    pthread_cond_broadcast(&g_bt_cond);
    pthread_mutex_unlock(&g_bt_mtx);
    return AIC_OK;
}

void aic_bt_detach(void)
{
    pthread_mutex_lock(&g_bt_mtx);
    g_bt_attached = 0;
    g_bt_listen_fd = -1;
    g_bt_incoming_fd = -1;
    g_bt_conn_fd = -1;
    g_bt_pending_out = 0;
    g_bt_in_head = 0;
    g_bt_in_len = 0;
    pthread_cond_broadcast(&g_bt_cond);
    pthread_mutex_unlock(&g_bt_mtx);
}

int aic_bt_attached(void)
{
    pthread_mutex_lock(&g_bt_mtx);
    int v = g_bt_attached;
    pthread_mutex_unlock(&g_bt_mtx);
    return v;
}

int aic_bt_feed(const void *data, size_t len)
{
    if (!data) return AIC_E_INVAL;
    bt_ring_push(data, len);
    pthread_mutex_lock(&g_bt_mtx);
    size_t free_space = AIC_TRANSPORT_RING_CAP - g_bt_in_len;
    pthread_mutex_unlock(&g_bt_mtx);
    return (int)free_space;
}

int aic_bt_drain(void *buf, size_t cap, size_t *out_n)
{
    if (out_n) *out_n = 0;
    if (!buf || !cap) return AIC_E_INVAL;
    pthread_mutex_lock(&g_bt_mtx);
    size_t take = g_bt_out_len < cap ? g_bt_out_len : cap;
    if (take) {
        memcpy(buf, g_bt_out, take);
        memmove(g_bt_out, g_bt_out + take, g_bt_out_len - take);
        g_bt_out_len -= take;
        g_bt_pending_out = (int)g_bt_out_len;
    }
    pthread_mutex_unlock(&g_bt_mtx);
    if (out_n) *out_n = take;
    return AIC_OK;
}

int aic_bt_pending_out(void)
{
    pthread_mutex_lock(&g_bt_mtx);
    int v = g_bt_pending_out;
    pthread_mutex_unlock(&g_bt_mtx);
    return v;
}
/* ---------- 通用传输对象 ---------- */

struct aic_transport {
    aic_channel         ch;
    aic_transport_state state;
    int                 listener;
    int                 conn;
    int                 port;
};

const char *aic_channel_name(aic_channel ch)
{
    switch (ch) {
    case AIC_CH_LAN:       return "lan";
    case AIC_CH_BLUETOOTH: return "bluetooth";
    default:               return "unknown";
    }
}

int aic_transport_available(aic_channel ch)
{
    if (ch == AIC_CH_LAN) return 1;
    /* 蓝牙是否可用由 Java 侧在 attach 之前判断（权限、适配器开关），C 侧只声明能力。 */
    if (ch == AIC_CH_BLUETOOTH) return 1;
    return 0;
}

int aic_transport_create(aic_channel ch, aic_transport **out)
{
    if (!out) return AIC_E_INVAL;
    if (ch != AIC_CH_LAN && ch != AIC_CH_BLUETOOTH) return AIC_E_UNSUPPORTED;
    aic_transport *t = (aic_transport *)aic_xcalloc(1, sizeof(aic_transport));
    t->ch = ch;
    t->state = AIC_TRANSPORT_CLOSED;
    t->listener = -1;
    t->conn = -1;
    t->port = AIC_TRANSPORT_DEFAULT_PORT;
    *out = t;
    return AIC_OK;
}

void aic_transport_close(aic_transport *t)
{
    if (!t) return;
    if (t->ch == AIC_CH_BLUETOOTH) aic_bt_detach();
    if (t->conn >= 0) { close(t->conn); t->conn = -1; }
    if (t->listener >= 0) { close(t->listener); t->listener = -1; }
    t->state = AIC_TRANSPORT_CLOSED;
}

void aic_transport_destroy(aic_transport *t)
{
    if (!t) return;
    aic_transport_close(t);
    aic_xfree(t);
}

aic_transport_state aic_transport_get_state(const aic_transport *t)
{
    return t ? t->state : AIC_TRANSPORT_CLOSED;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    p.revents = 0;
    int rc = poll(&p, 1, timeout_ms);
    return rc > 0 ? 1 : (rc == 0 ? 0 : -1);
}

int aic_transport_listen(aic_transport *t, int port, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t) return AIC_E_INVAL;
    aic_transport_close(t);
    if (t->ch == AIC_CH_BLUETOOTH) {
        /* Java 侧建立 BluetoothServerSocket 后调用 aicBtAttachListenFd 登记 fd。 */
        t->port = port;
        t->state = AIC_TRANSPORT_LISTENING;
        return AIC_OK;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        if (err && errlen) snprintf(err, errlen, "创建监听套接字失败（errno %d）", errno);
        return AIC_E_IO;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(s);
        if (err && errlen) snprintf(err, errlen, "端口 %d 无法监听（errno %d），可能已被占用", port, errno);
        return AIC_E_IO;
    }
    if (listen(s, 8) != 0) {
        close(s);
        if (err && errlen) snprintf(err, errlen, "监听失败（errno %d）", errno);
        return AIC_E_IO;
    }
    t->listener = s;
    t->port = port;
    t->state = AIC_TRANSPORT_LISTENING;
    return AIC_OK;
}

int aic_transport_accept(aic_transport *t, int timeout_ms, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t) return AIC_E_INVAL;
    if (t->ch == AIC_CH_BLUETOOTH) {
        /* 等 Java 侧 BluetoothServerSocket.accept() 成功后登记进来的连接。 */
        long long waited = 0;
        while (waited <= timeout_ms) {
            pthread_mutex_lock(&g_bt_mtx);
            int have = g_bt_incoming_fd >= 0;
            pthread_mutex_unlock(&g_bt_mtx);
            if (have) {
                t->conn = 0; /* 连接由 Java 持有，C 侧只做字节流转发 */
                t->state = AIC_TRANSPORT_CONNECTED;
                return AIC_OK;
            }
            usleep(50 * 1000);
            waited += 50;
        }
        if (err && errlen) snprintf(err, errlen, "等待对端蓝牙连接超时");
        return AIC_E_TIMEOUT;
    }
    if (t->listener < 0) return AIC_E_STATE;
    int ready = wait_fd(t->listener, POLLIN, timeout_ms);
    if (ready <= 0) {
        if (err && errlen) snprintf(err, errlen, ready == 0 ? "等待对端连接超时" : "等待连接出错");
        return ready == 0 ? AIC_E_TIMEOUT : AIC_E_IO;
    }
    int c = accept(t->listener, NULL, NULL);
    if (c < 0) {
        if (err && errlen) snprintf(err, errlen, "接受连接失败（errno %d）", errno);
        return AIC_E_IO;
    }
    t->conn = c;
    t->state = AIC_TRANSPORT_CONNECTED;
    return AIC_OK;
}

int aic_transport_connect(aic_transport *t, const char *host, int port, int timeout_ms,
                          char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t || !host) return AIC_E_INVAL;
    aic_transport_close(t);
    if (t->ch == AIC_CH_BLUETOOTH) {
        /* 蓝牙连接由 Java 的 BluetoothDevice.createRfcommSocketToServiceRecord 完成，
         * 建成后调用 aicBtAttachFd 登记；这里按超时等它出现。 */
        long long waited = 0;
        int wait_ms = timeout_ms > 0 ? timeout_ms : 15000;
        while (waited <= wait_ms) {
            if (aic_bt_attached()) {
                t->conn = 0;
                t->port = port;
                t->state = AIC_TRANSPORT_CONNECTED;
                return AIC_OK;
            }
            usleep(50 * 1000);
            waited += 50;
        }
        if (err && errlen) snprintf(err, errlen, "蓝牙连接 %s 超时（%d 毫秒）", host, wait_ms);
        return AIC_E_TIMEOUT;
    }

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (err && errlen) snprintf(err, errlen, "无法解析对端地址 %s", host);
        return AIC_E_IO;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        if (err && errlen) snprintf(err, errlen, "创建套接字失败（errno %d）", errno);
        return AIC_E_IO;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
        if (wait_fd(s, POLLOUT, timeout_ms) <= 0) {
            close(s);
            freeaddrinfo(res);
            if (err && errlen) snprintf(err, errlen, "连接 %s:%d 超时（%d 毫秒）", host, port, timeout_ms);
            return AIC_E_TIMEOUT;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
            close(s);
            freeaddrinfo(res);
            if (err && errlen) snprintf(err, errlen, "连接 %s:%d 失败（errno %d）", host, port, soerr);
            return AIC_E_IO;
        }
    } else if (rc != 0) {
        close(s);
        freeaddrinfo(res);
        if (err && errlen) snprintf(err, errlen, "连接 %s:%d 失败（errno %d）", host, port, errno);
        return AIC_E_IO;
    }
    fcntl(s, F_SETFL, fl);
    freeaddrinfo(res);
    t->conn = s;
    t->port = port;
    t->state = AIC_TRANSPORT_CONNECTED;
    return AIC_OK;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* 蓝牙发送：把整帧塞进输出队列，Java 侧读出来后写进 BluetoothSocket。 */
static int bt_send(const void *data, size_t len, char *err, size_t errlen)
{
    if (len > AIC_TRANSPORT_MAX_FRAME) {
        if (err && errlen) snprintf(err, errlen, "单帧超过上限（%u 字节）", AIC_TRANSPORT_MAX_FRAME);
        return AIC_E_LIMIT;
    }
    pthread_mutex_lock(&g_bt_mtx);
    if (!g_bt_attached) {
        pthread_mutex_unlock(&g_bt_mtx);
        if (err && errlen) snprintf(err, errlen, "蓝牙链路未建立");
        return AIC_E_STATE;
    }
    if (g_bt_out_len + 4 + len > BT_OUT_CAP) {
        pthread_mutex_unlock(&g_bt_mtx);
        if (err && errlen) snprintf(err, errlen, "蓝牙发送缓冲已满，稍后重试");
        return AIC_E_BUSY;
    }
    unsigned char hdr[4];
    hdr[0] = (unsigned char)((len >> 24) & 0xFF);
    hdr[1] = (unsigned char)((len >> 16) & 0xFF);
    hdr[2] = (unsigned char)((len >> 8) & 0xFF);
    hdr[3] = (unsigned char)(len & 0xFF);
    memcpy(g_bt_out + g_bt_out_len, hdr, 4);
    g_bt_out_len += 4;
    if (len) {
        memcpy(g_bt_out + g_bt_out_len, data, len);
        g_bt_out_len += len;
    }
    g_bt_pending_out = (int)g_bt_out_len;
    pthread_mutex_unlock(&g_bt_mtx);
    return AIC_OK;
}

int aic_transport_send(aic_transport *t, const void *data, size_t len, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t) return AIC_E_INVAL;
    if (t->ch == AIC_CH_BLUETOOTH) {
        if (t->state != AIC_TRANSPORT_CONNECTED) return AIC_E_STATE;
        return bt_send(data, len, err, errlen);
    }
    if (t->conn < 0) return AIC_E_STATE;
    if (len > AIC_TRANSPORT_MAX_FRAME) {
        if (err && errlen) snprintf(err, errlen, "单帧超过上限（%u 字节）", AIC_TRANSPORT_MAX_FRAME);
        return AIC_E_LIMIT;
    }
    unsigned char hdr[4];
    hdr[0] = (unsigned char)((len >> 24) & 0xFF);
    hdr[1] = (unsigned char)((len >> 16) & 0xFF);
    hdr[2] = (unsigned char)((len >> 8) & 0xFF);
    hdr[3] = (unsigned char)(len & 0xFF);
    if (send_all(t->conn, (const char *)hdr, 4) != 0 || (len && send_all(t->conn, (const char *)data, len) != 0)) {
        if (err && errlen) snprintf(err, errlen, "发送失败（errno %d），链路可能已断开", errno);
        aic_transport_close(t);
        return AIC_E_IO;
    }
    return AIC_OK;
}

static int recv_all(int fd, char *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    while (got < len) {
        if (wait_fd(fd, POLLIN, timeout_ms) <= 0) return -1;
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* 蓝牙接收：从 Java 灌进来的环形缓冲里取整帧。 */
static int bt_recv(char **out, size_t *out_len, int timeout_ms, char *err, size_t errlen)
{
    unsigned char hdr[4];
    if (bt_ring_pop(hdr, 4, timeout_ms) != 4) {
        if (err && errlen) snprintf(err, errlen, "等待对端数据超时");
        return AIC_E_TIMEOUT;
    }
    size_t len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | (size_t)hdr[3];
    if (len > AIC_TRANSPORT_MAX_FRAME) {
        if (err && errlen) snprintf(err, errlen, "收到超长帧（%zu 字节）", len);
        return AIC_E_FORMAT;
    }
    char *buf = (char *)aic_xmalloc(len + 1);
    if (len && bt_ring_pop(buf, len, timeout_ms) != len) {
        aic_xfree(buf);
        if (err && errlen) snprintf(err, errlen, "蓝牙帧不完整，链路可能已断开");
        return AIC_E_IO;
    }
    buf[len] = '\0';
    if (out) *out = buf;
    else aic_xfree(buf);
    if (out_len) *out_len = len;
    return AIC_OK;
}

int aic_transport_recv(aic_transport *t, char **out, size_t *out_len, int timeout_ms,
                       char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!t) return AIC_E_INVAL;
    if (t->ch == AIC_CH_BLUETOOTH) {
        if (t->state != AIC_TRANSPORT_CONNECTED) return AIC_E_STATE;
        return bt_recv(out, out_len, timeout_ms, err, errlen);
    }
    if (t->conn < 0) return AIC_E_STATE;

    int ready = wait_fd(t->conn, POLLIN, timeout_ms);
    if (ready == 0) {
        if (err && errlen) snprintf(err, errlen, "等待对端数据超时");
        return AIC_E_TIMEOUT;
    }
    if (ready < 0) return AIC_E_IO;

    unsigned char hdr[4];
    if (recv_all(t->conn, (char *)hdr, 4, 0) != 0) {
        if (err && errlen) snprintf(err, errlen, "链路已断开");
        aic_transport_close(t);
        return AIC_E_IO;
    }
    size_t len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | (size_t)hdr[3];
    if (len > AIC_TRANSPORT_MAX_FRAME) {
        if (err && errlen) snprintf(err, errlen, "收到超长帧（%zu 字节）", len);
        return AIC_E_FORMAT;
    }
    char *buf = (char *)aic_xmalloc(len + 1);
    if (len && recv_all(t->conn, buf, len, timeout_ms) != 0) {
        aic_xfree(buf);
        if (err && errlen) snprintf(err, errlen, "接收数据失败");
        aic_transport_close(t);
        return AIC_E_IO;
    }
    buf[len] = '\0';
    if (out) *out = buf;
    else aic_xfree(buf);
    if (out_len) *out_len = len;
    return AIC_OK;
}

int aic_transport_local_ip(char *out, size_t cap)
{
    if (!out || !cap) return AIC_E_INVAL;
    out[0] = '\0';
    /* 用 UDP 连接探测默认出口地址，不真正发包。 */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return AIC_E_IO;
    struct sockaddr_in probe;
    memset(&probe, 0, sizeof(probe));
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    probe.sin_addr.s_addr = inet_addr("8.8.8.8");
    int ok = 0;
    if (connect(s, (struct sockaddr *)&probe, sizeof(probe)) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
            if (inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)cap)) ok = 1;
        }
    }
    close(s);
    return ok ? AIC_OK : AIC_E_NOTFOUND;
}

int aic_transport_bluetooth_peers(aic_peer *out, size_t cap, size_t *out_n,
                                  char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_n) *out_n = 0;
    if (!out || !cap) return AIC_E_INVAL;
    /* 已配对设备列表由 Java 的 BluetoothAdapter.getBondedDevices 提供，
     * 通过 aicBtSetPeers 一次性推给 C 侧，这里只读缓存。 */
    return aic_bt_peers_cache(out, cap, out_n);
}

/* Java 侧把已配对设备推进来的缓存（见 aic_jni.c 的 aicBtSetPeers 命令）。 */
static aic_peer g_bt_peers[AIC_TRANSPORT_PEER_MAX];
static size_t   g_bt_peer_n = 0;

int aic_bt_peers_set(const aic_peer *list, size_t n)
{
    if (n > AIC_TRANSPORT_PEER_MAX) n = AIC_TRANSPORT_PEER_MAX;
    g_bt_peer_n = 0;
    for (size_t i = 0; i < n; i++) {
        g_bt_peers[i] = list[i];
        g_bt_peers[i].channel = AIC_CH_BLUETOOTH;
        if (g_bt_peers[i].port <= 0) g_bt_peers[i].port = AIC_BT_CHANNEL_MIN;
    }
    g_bt_peer_n = n;
    return AIC_OK;
}

int aic_bt_peers_cache(aic_peer *out, size_t cap, size_t *out_n)
{
    if (out_n) *out_n = 0;
    if (!out || !cap) return AIC_E_INVAL;
    size_t n = g_bt_peer_n < cap ? g_bt_peer_n : cap;
    for (size_t i = 0; i < n; i++) out[i] = g_bt_peers[i];
    if (out_n) *out_n = n;
    return n ? AIC_OK : AIC_E_NOTFOUND;
}

int aic_transport_discover(int port, int wait_ms, aic_peer *out, size_t cap, size_t *out_n,
                           char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_n) *out_n = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        if (err && errlen) snprintf(err, errlen, "创建 UDP 套接字失败（errno %d）", errno);
        return AIC_E_IO;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    dst.sin_port = htons((unsigned short)port);
    sendto(s, AIC_DISCOVER_MAGIC, strlen(AIC_DISCOVER_MAGIC), 0, (struct sockaddr *)&dst, sizeof(dst));

    size_t n = 0;
    for (;;) {
        if (n >= cap) break;
        if (wait_fd(s, POLLIN, wait_ms) <= 0) break;
        char buf[512];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t got = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (got <= 0) break;
        buf[got] = '\0';
        if (strncmp(buf, AIC_DISCOVER_REPLY, strlen(AIC_DISCOVER_REPLY)) != 0) continue;
        char *p = buf + strlen(AIC_DISCOVER_REPLY);
        while (*p == ' ') p++;
        char id[40];
        size_t i = 0;
        while (*p && *p != ' ' && i + 1 < sizeof(id)) id[i++] = *p++;
        id[i] = '\0';
        while (*p == ' ') p++;
        aic_strlcpy_utf8(out[n].device_id, sizeof(out[n].device_id), id);
        aic_strlcpy_utf8(out[n].name, sizeof(out[n].name), p);
        inet_ntop(AF_INET, &from.sin_addr, out[n].addr, sizeof(out[n].addr));
        out[n].port = port;
        out[n].channel = AIC_CH_LAN;
        n++;
    }
    close(s);
    if (out_n) *out_n = n;
    return n ? AIC_OK : AIC_E_NOTFOUND;
}

int aic_transport_respond(int port, int wait_ms, const char *device_id, const char *name,
                          int *out_answered, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_answered) *out_answered = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return AIC_E_IO;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(s);
        if (err && errlen) snprintf(err, errlen, "UDP 端口 %d 无法绑定（errno %d）", port, errno);
        return AIC_E_IO;
    }
    char reply[256];
    snprintf(reply, sizeof(reply), "%s %s %s", AIC_DISCOVER_REPLY,
             device_id ? device_id : "", name ? name : "");
    int answered = 0;
    if (wait_fd(s, POLLIN, wait_ms) > 0) {
        char buf[256];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t got = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (got > 0) {
            buf[got] = '\0';
            if (strncmp(buf, AIC_DISCOVER_MAGIC, strlen(AIC_DISCOVER_MAGIC)) == 0) {
                sendto(s, reply, strlen(reply), 0, (struct sockaddr *)&from, from_len);
                answered = 1;
            }
        }
    }
    close(s);
    if (out_answered) *out_answered = answered;
    return AIC_OK;
}