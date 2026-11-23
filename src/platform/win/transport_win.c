/* transport_win.c —— Windows 传输适配层（Winsock2）：局域网 TCP 直连 + UDP 发现，
 * 以及蓝牙 RFCOMM 串行通道（AF_BTH + BTHPROTO_RFCOMM，技术设计 §16.1）。
 *
 * 两条通道共用同一套帧格式与上层同步逻辑，只有套接字族、连接参数与发现方式不同：
 * 局域网用 UDP 广播发现，蓝牙用系统已配对设备列表（BluetoothFindFirstDevice）。 */
#include "transport.h"

#include <winsock2.h>
#include <ws2bth.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bluetoothapis.h>

#include <stdio.h>
#include <string.h>

struct aic_transport {
    aic_channel         ch;
    aic_transport_state state;
    SOCKET              listener; /* 局域网监听套接字 */
    SOCKET              conn;     /* 已建立的连接 */
    int                 port;
    int                 rfcomm_channel;  /* 蓝牙专用：RFCOMM 通道号 */
    BTH_ADDR            rfcomm_addr;     /* 蓝牙专用：对端蓝牙地址 */
};

const char *aic_channel_name(aic_channel ch)
{
    switch (ch) {
    case AIC_CH_LAN:         return "lan";
    case AIC_CH_BLUETOOTH:   return "bluetooth";
    default:                 return "unknown";
    }
}

static int wsa_ready(void)
{
    static int inited = 0;
    static int ok = 0;
    if (!inited) {
        WSADATA wd;
        ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
        inited = 1;
    }
    return ok;
}

/* ---------- 蓝牙：地址与无线电状态 ---------- */

int aic_bt_parse_addr(const char *text, unsigned long long *out_addr)
{
    if (out_addr) *out_addr = 0;
    if (!text || !*text) return AIC_E_INVAL;
    unsigned long long v = 0;
    int digits = 0;
    for (const char *p = text; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return AIC_E_INVAL;
        v = (v << 4) | (unsigned long long)d;
        digits++;
    }
    if (digits != 12) return AIC_E_INVAL;
    if (out_addr) *out_addr = v;
    return AIC_OK;
}

static void bt_addr_text(BTH_ADDR a, char *out, size_t cap)
{
    snprintf(out, cap, "%02llX:%02llX:%02llX:%02llX:%02llX:%02llX",
             (unsigned long long)((a >> 40) & 0xFF),
             (unsigned long long)((a >> 32) & 0xFF),
             (unsigned long long)((a >> 24) & 0xFF),
             (unsigned long long)((a >> 16) & 0xFF),
             (unsigned long long)((a >> 8) & 0xFF),
             (unsigned long long)(a & 0xFF));
}

static void request_bt_radio(void)
{
    /* 无线电被关掉时蓝牙查找会直接失败，先请求一次打开（系统会弹授权，用户拒绝也不影响其余通道）。 */
    HMODULE lib = LoadLibraryW(L"bthprops.cpl");
    if (!lib) return;
    FARPROC fn = GetProcAddress(lib, "BluetoothEnableDiscovery");
    if (fn) {
        typedef int (WINAPI *pbth_turn_on_t)(void *);
        ((pbth_turn_on_t)(void *)fn)(NULL);
    }
    FreeLibrary(lib);
}

int aic_bt_radio_ready(char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!wsa_ready()) {
        if (err && errlen) snprintf(err, errlen, "网络栈不可用");
        return AIC_E_UNSUPPORTED;
    }
    BLUETOOTH_FIND_RADIO_PARAMS rp;
    memset(&rp, 0, sizeof(rp));
    rp.dwSize = sizeof(rp);
    HANDLE hradio = NULL;
    HBLUETOOTH_RADIO_FIND rf = BluetoothFindFirstRadio(&rp, &hradio);
    if (!rf) {
        request_bt_radio();
        DWORD e = GetLastError();
        if (err && errlen) {
            if (e == ERROR_NO_MORE_ITEMS) snprintf(err, errlen, "这台电脑没有可用的蓝牙无线电");
            else if (e == ERROR_NOT_READY) snprintf(err, errlen, "蓝牙未开启，请在系统设置里打开蓝牙");
            else snprintf(err, errlen, "蓝牙无线电不可用（错误码 %lu）", (unsigned long)e);
        }
        return AIC_E_UNSUPPORTED;
    }
    if (hradio) CloseHandle(hradio);
    BluetoothFindRadioClose(rf);
    return AIC_OK;
}

int aic_transport_bluetooth_peers(aic_peer *out, size_t cap, size_t *out_n,
                                  char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_n) *out_n = 0;
    if (!out || !cap) return AIC_E_INVAL;
    if (!wsa_ready()) return AIC_E_UNSUPPORTED;

    BLUETOOTH_DEVICE_SEARCH_PARAMS sp;
    memset(&sp, 0, sizeof(sp));
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;  /* 只用已配对设备：未配对的还需要用户在系统里确认 */
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;        /* 不做主动搜索，避免打扰周围设备 */
    sp.cTimeoutMultiplier = 2;
    sp.hRadio = NULL;

    BLUETOOTH_DEVICE_INFO di;
    memset(&di, 0, sizeof(di));
    di.dwSize = sizeof(di);
    HBLUETOOTH_DEVICE_FIND fd = BluetoothFindFirstDevice(&sp, &di);
    if (!fd) {
        DWORD e = GetLastError();
        if (e == ERROR_NO_MORE_ITEMS) {
            if (err && errlen) snprintf(err, errlen, "没有已配对的蓝牙设备，请先在系统设置里配对");
            return AIC_E_NOTFOUND;
        }
        if (err && errlen) snprintf(err, errlen, "枚举蓝牙设备失败（错误码 %lu）", (unsigned long)e);
        return AIC_E_IO;
    }
    size_t n = 0;
    do {
        if (n >= cap) break;
        char addr[AIC_BT_ADDR_MAX];
        bt_addr_text(di.Address.ullLong, addr, sizeof(addr));
        aic_strlcpy_utf8(out[n].device_id, sizeof(out[n].device_id), addr);
        /* szName 是 UTF-16，先转 UTF-8 再存。 */
        WideCharToMultiByte(CP_UTF8, 0, di.szName, -1, out[n].name, (int)sizeof(out[n].name), NULL, NULL);
        aic_strlcpy_utf8(out[n].addr, sizeof(out[n].addr), addr);
        out[n].port = AIC_BT_CHANNEL_MIN;
        out[n].channel = AIC_CH_BLUETOOTH;
        n++;
    } while (BluetoothFindNextDevice(fd, &di));
    BluetoothFindDeviceClose(fd);
    if (out_n) *out_n = n;
    return n ? AIC_OK : AIC_E_NOTFOUND;
}

/* ---------- 通用 ---------- */

int aic_transport_available(aic_channel ch)
{
    if (ch == AIC_CH_LAN) return wsa_ready();
    if (ch == AIC_CH_BLUETOOTH) return aic_bt_radio_ready(NULL, 0) == AIC_OK;
    return 0;
}

int aic_transport_create(aic_channel ch, aic_transport **out)
{
    if (!out) return AIC_E_INVAL;
    if (!wsa_ready()) return AIC_E_UNSUPPORTED;
    /* 蓝牙预留：无线电没开也允许创建对象，具体失败由 listen / connect 报出来。 */
    if (ch != AIC_CH_LAN && ch != AIC_CH_BLUETOOTH) return AIC_E_UNSUPPORTED;
    aic_transport *t = (aic_transport *)aic_xcalloc(1, sizeof(aic_transport));
    t->ch = ch;
    t->state = AIC_TRANSPORT_CLOSED;
    t->listener = INVALID_SOCKET;
    t->conn = INVALID_SOCKET;
    t->port = AIC_TRANSPORT_DEFAULT_PORT;
    t->rfcomm_channel = AIC_BT_CHANNEL_MIN;
    *out = t;
    return AIC_OK;
}

void aic_transport_close(aic_transport *t)
{
    if (!t) return;
    if (t->conn != INVALID_SOCKET) { closesocket(t->conn); t->conn = INVALID_SOCKET; }
    if (t->listener != INVALID_SOCKET) { closesocket(t->listener); t->listener = INVALID_SOCKET; }
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

static int wait_readable(SOCKET s, int timeout_ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(0, &rd, NULL, NULL, &tv);
    return rc > 0 ? 1 : (rc == 0 ? 0 : -1);
}

/* 把 SOCKADDR_BTH 填好：蓝牙地址 + RFCOMM 通道号。 */
static void fill_bth_addr(SOCKADDR_BTH *addr, BTH_ADDR dev, int channel)
{
    memset(addr, 0, sizeof(*addr));
    addr->addressFamily = AF_BTH;
    addr->btAddr = dev;
    addr->port = (ULONG)channel;
}

static int bt_listen(aic_transport *t, int port, char *err, size_t errlen)
{
    int ch = port > 0 ? port : AIC_BT_CHANNEL_MIN;
    if (ch < AIC_BT_CHANNEL_MIN || ch > AIC_BT_CHANNEL_MAX) {
        if (err && errlen) snprintf(err, errlen, "RFCOMM 通道号 %d 超出范围（%d~%d）", ch,
                                    AIC_BT_CHANNEL_MIN, AIC_BT_CHANNEL_MAX);
        return AIC_E_INVAL;
    }
    char rerr[256];
    rerr[0] = '\0';
    int rc = aic_bt_radio_ready(rerr, sizeof(rerr));
    if (rc != AIC_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", rerr[0] ? rerr : "蓝牙不可用");
        return rc;
    }
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) {
        if (err && errlen) snprintf(err, errlen, "创建蓝牙套接字失败（错误码 %d）", WSAGetLastError());
        return AIC_E_IO;
    }
    SOCKADDR_BTH sa;
    fill_bth_addr(&sa, 0, ch); /* 本端地址填 0，由协议栈分配 */
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        if (err && errlen) snprintf(err, errlen, "RFCOMM 通道 %d 无法绑定（错误码 %d），可能被其它程序占用", ch, e);
        return AIC_E_IO;
    }
    if (listen(s, SOMAXCONN) != 0) {
        closesocket(s);
        if (err && errlen) snprintf(err, errlen, "蓝牙监听失败（错误码 %d）", WSAGetLastError());
        return AIC_E_IO;
    }
    t->listener = s;
    t->port = ch;
    t->rfcomm_channel = ch;
    t->state = AIC_TRANSPORT_LISTENING;
    return AIC_OK;
}

int aic_transport_listen(aic_transport *t, int port, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t) return AIC_E_INVAL;
    aic_transport_close(t);
    if (t->ch == AIC_CH_BLUETOOTH) return bt_listen(t, port, err, errlen);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (err && errlen) snprintf(err, errlen, "创建监听套接字失败（错误码 %d）", WSAGetLastError());
        return AIC_E_IO;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        if (err && errlen) snprintf(err, errlen, "端口 %d 无法监听（错误码 %d），可能已被占用", port, e);
        return AIC_E_IO;
    }
    if (listen(s, SOMAXCONN) != 0) {
        closesocket(s);
        if (err && errlen) snprintf(err, errlen, "监听失败（错误码 %d）", WSAGetLastError());
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
    if (!t || t->listener == INVALID_SOCKET) return AIC_E_STATE;
    int ready = wait_readable(t->listener, timeout_ms);
    if (ready <= 0) {
        if (err && errlen) snprintf(err, errlen, ready == 0 ? "等待对端连接超时" : "等待连接出错");
        return ready == 0 ? AIC_E_TIMEOUT : AIC_E_IO;
    }
    SOCKET c = accept(t->listener, NULL, NULL);
    if (c == INVALID_SOCKET) {
        if (err && errlen) snprintf(err, errlen, "接受连接失败（错误码 %d）", WSAGetLastError());
        return AIC_E_IO;
    }
    t->conn = c;
    t->state = AIC_TRANSPORT_CONNECTED;
    return AIC_OK;
}

static int bt_connect(aic_transport *t, const char *host, int port, int timeout_ms,
                      char *err, size_t errlen)
{
    unsigned long long dev = 0;
    if (aic_bt_parse_addr(host, &dev) != AIC_OK) {
        /* 也允许直接传设备名：到已配对设备里找同名的一项。 */
        aic_peer list[16];
        size_t n = 0;
        if (aic_transport_bluetooth_peers(list, 16, &n, NULL, 0) == AIC_OK) {
            for (size_t i = 0; i < n; i++) {
                if (aic_str_ieq(list[i].name, host)) {
                    aic_bt_parse_addr(list[i].addr, &dev);
                    break;
                }
            }
        }
        if (dev == 0) {
            if (err && errlen) snprintf(err, errlen, "蓝牙地址或设备名无法识别：%s", host);
            return AIC_E_INVAL;
        }
    }
    int ch = port >= AIC_BT_CHANNEL_MIN && port <= AIC_BT_CHANNEL_MAX ? port : AIC_BT_CHANNEL_MIN;
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) {
        if (err && errlen) snprintf(err, errlen, "创建蓝牙套接字失败（错误码 %d）", WSAGetLastError());
        return AIC_E_IO;
    }
    /* 蓝牙套接字不支持非阻塞 connect，改由上层超时：这里直接阻塞，
     * 超时由系统协议栈决定（通常 10 秒内返回失败）。 */
    (void)timeout_ms;
    SOCKADDR_BTH sa;
    fill_bth_addr(&sa, (BTH_ADDR)dev, ch);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        if (err && errlen) snprintf(err, errlen, "蓝牙连接 %s 通道 %d 失败（错误码 %d）", host, ch, e);
        return AIC_E_IO;
    }
    t->conn = s;
    t->port = ch;
    t->rfcomm_channel = ch;
    t->rfcomm_addr = (BTH_ADDR)dev;
    t->state = AIC_TRANSPORT_CONNECTED;
    return AIC_OK;
}

int aic_transport_connect(aic_transport *t, const char *host, int port, int timeout_ms,
                          char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t || !host) return AIC_E_INVAL;
    aic_transport_close(t);
    if (t->ch == AIC_CH_BLUETOOTH) return bt_connect(t, host, port, timeout_ms, err, errlen);

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (err && errlen) snprintf(err, errlen, "无法解析对端地址 %s", host);
        return AIC_E_IO;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        if (err && errlen) snprintf(err, errlen, "创建套接字失败（错误码 %d）", WSAGetLastError());
        return AIC_E_IO;
    }

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    if (rc != 0) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) {
            closesocket(s);
            freeaddrinfo(res);
            if (err && errlen) snprintf(err, errlen, "连接 %s:%d 失败（错误码 %d）", host, port, e);
            return AIC_E_IO;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, NULL, &wr, NULL, &tv) <= 0) {
            closesocket(s);
            freeaddrinfo(res);
            if (err && errlen) snprintf(err, errlen, "连接 %s:%d 超时（%d 毫秒）", host, port, timeout_ms);
            return AIC_E_TIMEOUT;
        }
        int soerr = 0;
        int len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
        if (soerr != 0) {
            closesocket(s);
            freeaddrinfo(res);
            if (err && errlen) snprintf(err, errlen, "连接 %s:%d 失败（错误码 %d）", host, port, soerr);
            return AIC_E_IO;
        }
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    freeaddrinfo(res);

    t->conn = s;
    t->port = port;
    t->state = AIC_TRANSPORT_CONNECTED;
    return AIC_OK;
}

static int send_all(SOCKET s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

int aic_transport_send(aic_transport *t, const void *data, size_t len, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!t || t->conn == INVALID_SOCKET) return AIC_E_STATE;
    if (len > AIC_TRANSPORT_MAX_FRAME) {
        if (err && errlen) snprintf(err, errlen, "单帧超过上限（%u 字节）", AIC_TRANSPORT_MAX_FRAME);
        return AIC_E_LIMIT;
    }
    unsigned char hdr[4];
    hdr[0] = (unsigned char)((len >> 24) & 0xFF);
    hdr[1] = (unsigned char)((len >> 16) & 0xFF);
    hdr[2] = (unsigned char)((len >> 8) & 0xFF);
    hdr[3] = (unsigned char)(len & 0xFF);
    if (send_all(t->conn, (const char *)hdr, 4) != 0 || (len && send_all(t->conn, (const char *)data, (int)len) != 0)) {
        if (err && errlen) snprintf(err, errlen, "发送失败（错误码 %d），链路可能已断开", WSAGetLastError());
        aic_transport_close(t);
        return AIC_E_IO;
    }
    return AIC_OK;
}

static int recv_all(SOCKET s, char *buf, int len, int timeout_ms)
{
    int got = 0;
    while (got < len) {
        int ready = wait_readable(s, timeout_ms);
        if (ready <= 0) return ready == 0 ? -2 : -1;
        int n = recv(s, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

int aic_transport_recv(aic_transport *t, char **out, size_t *out_len, int timeout_ms,
                       char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!t || t->conn == INVALID_SOCKET) return AIC_E_STATE;

    unsigned char hdr[4];
    int rc = recv_all(t->conn, (char *)hdr, 4, timeout_ms);
    if (rc == -2) {
        if (err && errlen) snprintf(err, errlen, "等待对端数据超时");
        return AIC_E_TIMEOUT;
    }
    if (rc != 0) {
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
    if (len && recv_all(t->conn, buf, (int)len, timeout_ms) != 0) {
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
    if (!wsa_ready()) return AIC_E_UNSUPPORTED;
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return AIC_E_IO;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return AIC_E_IO;
    int ok = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ai_addr;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            /* 跳过 127.x 回环地址。 */
            if (strncmp(buf, "127.", 4) != 0) {
                aic_strlcpy_utf8(out, cap, buf);
                ok = 1;
                break;
            }
        }
    }
    freeaddrinfo(res);
    return ok ? AIC_OK : AIC_E_NOTFOUND;
}

int aic_transport_discover(int port, int wait_ms, aic_peer *out, size_t cap, size_t *out_n,
                           char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_n) *out_n = 0;
    if (!wsa_ready()) {
        if (err && errlen) snprintf(err, errlen, "网络栈不可用");
        return AIC_E_UNSUPPORTED;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return AIC_E_IO;
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_BROADCAST;
    dst.sin_port = htons((unsigned short)port);
    sendto(s, AIC_DISCOVER_MAGIC, (int)strlen(AIC_DISCOVER_MAGIC), 0,
           (struct sockaddr *)&dst, sizeof(dst));

    size_t n = 0;
    DWORD start = GetTickCount();
    while (n < cap) {
        DWORD spent = GetTickCount() - start;
        if (spent >= (DWORD)wait_ms) break;
        int left = wait_ms - (int)spent;
        if (wait_readable(s, left) <= 0) break;
        char buf[512];
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int got = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (got <= 0) continue;
        buf[got] = '\0';
        /* 应答格式：SAI-HERE <device_id> <name...> */
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
        int dup = 0;
        for (size_t k = 0; k + 1 < n; k++) {
            if (strcmp(out[k].device_id, out[n - 1].device_id) == 0) dup = 1;
        }
        if (dup) {
            memset(&out[n - 1], 0, sizeof(out[n - 1]));
            n--;
        }
    }
    closesocket(s);
    if (out_n) *out_n = n;
    return n ? AIC_OK : AIC_E_NOTFOUND;
}

int aic_transport_respond(int port, int wait_ms, const char *device_id, const char *name,
                          int *out_answered, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_answered) *out_answered = 0;
    if (!wsa_ready()) return AIC_E_UNSUPPORTED;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return AIC_E_IO;
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        if (err && errlen) snprintf(err, errlen, "UDP 端口 %d 无法绑定（错误码 %d）", port, WSAGetLastError());
        return AIC_E_IO;
    }
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    char reply[256];
    snprintf(reply, sizeof(reply), "%s %s %s", AIC_DISCOVER_REPLY,
             device_id ? device_id : "", name ? name : "");
    int answered = 0;
    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < wait_ms) {
        int left = wait_ms - (int)(GetTickCount() - start);
        if (left <= 0) break;
        if (wait_readable(s, left) <= 0) continue;
        char buf[256];
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int got = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (got <= 0) continue;
        buf[got] = '\0';
        if (strncmp(buf, AIC_DISCOVER_MAGIC, strlen(AIC_DISCOVER_MAGIC)) != 0) continue;
        sendto(s, reply, (int)strlen(reply), 0, (struct sockaddr *)&from, from_len);
        answered++;
    }
    closesocket(s);
    if (out_answered) *out_answered = answered;
    return AIC_OK;
}
