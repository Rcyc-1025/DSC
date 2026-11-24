/* sync_session.c —— 传输、配对、同步引擎的组合驱动（§16.1 至 §16.6）。 */
#include "sync_session.h"
#include "json.h"
#include "pairing.h"
#include "sync_clock.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

struct dsc_session {
    dsc_db         *db;
    dsc_transport  *tr;
    dsc_pairing    *pair;
    dsc_sync       *sync;
    dsc_sync_state  phase;
    char            peer_id[40];
    char            peer_name[64];
    int             pending_n;
    char            pending[DSC_SESSION_PENDING_MAX][DSC_SYNC_ID_MAX];
    int             sent_total;
    int             applied_total;
    int             want_channel;  /* -1 = 跟随默认（局域网优先） */
};

int dsc_session_create(dsc_db *db, dsc_session **out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_session *s = (dsc_session *)dsc_xcalloc(1, sizeof(dsc_session));
    s->db = db;
    int rc = dsc_sync_create(db, &s->sync);
    if (rc != DSC_OK) { dsc_xfree(s); return rc; }
    rc = dsc_pairing_create(db, &s->pair);
    if (rc != DSC_OK) { dsc_sync_destroy(s->sync); dsc_xfree(s); return rc; }
    s->phase = DSC_SYNC_IDLE;
    s->want_channel = -1;
    *out = s;
    return DSC_OK;
}

void dsc_session_close(dsc_session *s)
{
    if (!s) return;
    if (s->tr) dsc_transport_close(s->tr);
    s->phase = DSC_SYNC_IDLE;
    s->pending_n = 0;
}

void dsc_session_destroy(dsc_session *s)
{
    if (!s) return;
    dsc_session_close(s);
    if (s->tr) dsc_transport_destroy(s->tr);
    if (s->pair) dsc_pairing_destroy(s->pair);
    if (s->sync) dsc_sync_destroy(s->sync);
    dsc_xfree(s);
}

dsc_sync_state dsc_session_phase(const dsc_session *s) { return s ? s->phase : DSC_SYNC_IDLE; }
int dsc_session_is_paired(const dsc_session *s) { return s && s->phase == DSC_SYNC_SYNCING; }
const char *dsc_session_our_code(const dsc_session *s) { return s ? dsc_pairing_code(s->pair) : ""; }

/* 选通道：显式指定优先，未指定时局域网优先，局域网不可用再退回蓝牙（§16.1 两条通道统一抽象）。 */
static dsc_channel pick_channel(const dsc_session *s)
{
    if (s->want_channel == (int)DSC_CH_LAN || s->want_channel == (int)DSC_CH_BLUETOOTH) {
        return (dsc_channel)s->want_channel;
    }
    if (dsc_transport_available(DSC_CH_LAN)) return DSC_CH_LAN;
    return DSC_CH_BLUETOOTH;
}

static int ensure_transport(dsc_session *s, char *err, size_t errlen)
{
    if (s->tr) return DSC_OK;
    dsc_channel ch = pick_channel(s);
    int rc = dsc_transport_create(ch, &s->tr);
    if (rc != DSC_OK) {
        if (err && errlen) {
            snprintf(err, errlen, "%s通道不可用：%s", ch == DSC_CH_LAN ? "局域网" : "蓝牙", dsc_strerror(rc));
        }
        return rc;
    }
    return DSC_OK;
}

int dsc_session_use_channel(dsc_session *s, int channel)
{
    if (!s) return DSC_E_INVAL;
    if (channel != -1 && channel != (int)DSC_CH_LAN && channel != (int)DSC_CH_BLUETOOTH) {
        return DSC_E_INVAL;
    }
    if (s->phase != DSC_SYNC_IDLE) return DSC_E_STATE;
    if (s->tr) {
        /* 还没进入任何阶段时允许换介质：关掉旧对象重建。 */
        dsc_transport_destroy(s->tr);
        s->tr = NULL;
    }
    s->want_channel = channel;
    return DSC_OK;
}

static int phase_to(dsc_session *s, dsc_sync_state next, char *err, size_t errlen)
{
    char e2[256];
    e2[0] = '\0';
    int rc = dsc_sync_enter(s->sync, next, e2, sizeof(e2));
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", e2[0] ? e2 : dsc_strerror(rc));
        return rc;
    }
    s->phase = next;
    return DSC_OK;
}

int dsc_session_host(dsc_session *s, int port, const char *avoid_code, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return DSC_E_INVAL;
    int rc = ensure_transport(s, err, errlen);
    if (rc != DSC_OK) return rc;
    if (s->phase != DSC_SYNC_IDLE && s->phase != DSC_SYNC_PAIRING) {
        rc = phase_to(s, DSC_SYNC_IDLE, err, errlen);
        if (rc != DSC_OK) return rc;
    }
    rc = dsc_transport_listen(s->tr, port, err, errlen);
    if (rc != DSC_OK) return rc;
    char code[DSC_PAIR_CODE_LEN + 1];
    rc = dsc_pairing_generate(s->pair, avoid_code, dsc_now_ms(), code, sizeof(code), err, errlen);
    if (rc != DSC_OK) return rc;
    rc = phase_to(s, DSC_SYNC_DISCOVERING, err, errlen);
    if (rc != DSC_OK) return rc;
    rc = phase_to(s, DSC_SYNC_PAIRING, err, errlen);
    return rc;
}

int dsc_session_answer_discovery(dsc_session *s, int wait_ms, int *out_answered,
                                 char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return DSC_E_INVAL;
    if (out_answered) *out_answered = 0;
    char dev[40];
    int rc = dsc_device_id(s->db, dev, sizeof(dev));
    if (rc != DSC_OK) return rc;
    char name[64];
    dsc_device_name(s->db, name, sizeof(name));
    return dsc_transport_respond(DSC_TRANSPORT_DEFAULT_PORT, wait_ms, dev, name, out_answered, err, errlen);
}

int dsc_session_discover(dsc_session *s, int port, int wait_ms, dsc_peer *peers, size_t cap,
                         size_t *out_n, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return DSC_E_INVAL;
    int rc = ensure_transport(s, err, errlen);
    if (rc != DSC_OK) return rc;
    if (s->phase == DSC_SYNC_IDLE) {
        rc = phase_to(s, DSC_SYNC_DISCOVERING, err, errlen);
        if (rc != DSC_OK) return rc;
    }
    return dsc_transport_discover(port, wait_ms, peers, cap, out_n, err, errlen);
}

int dsc_session_connect(dsc_session *s, const char *addr, int port, int timeout_ms,
                        const char *avoid_code, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s || !addr) return DSC_E_INVAL;
    int rc = ensure_transport(s, err, errlen);
    if (rc != DSC_OK) return rc;
    if (s->phase == DSC_SYNC_IDLE) {
        rc = phase_to(s, DSC_SYNC_DISCOVERING, err, errlen);
        if (rc != DSC_OK) return rc;
    }
    if (s->phase == DSC_SYNC_DISCOVERING) {
        rc = phase_to(s, DSC_SYNC_CONNECTING, err, errlen);
        if (rc != DSC_OK) return rc;
    }
    rc = dsc_transport_connect(s->tr, addr, port, timeout_ms, err, errlen);
    if (rc != DSC_OK) {
        char e2[256];
        e2[0] = '\0';
        phase_to(s, DSC_SYNC_IDLE, e2, sizeof(e2));
        return rc;
    }
    char code[DSC_PAIR_CODE_LEN + 1];
    rc = dsc_pairing_generate(s->pair, avoid_code, dsc_now_ms(), code, sizeof(code), err, errlen);
    if (rc != DSC_OK) return rc;
    return phase_to(s, DSC_SYNC_PAIRING, err, errlen);
}

int dsc_session_accept(dsc_session *s, int timeout_ms, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s || !s->tr) return DSC_E_STATE;
    int rc = dsc_transport_accept(s->tr, timeout_ms, err, errlen);
    if (rc != DSC_OK) return rc;
    if (s->phase == DSC_SYNC_DISCOVERING) {
        rc = phase_to(s, DSC_SYNC_PAIRING, err, errlen);
    }
    return rc;
}

/* 帧收发小工具：发送一个 JSON 文本帧。 */
static int send_json(dsc_session *s, const char *json, char *err, size_t errlen)
{
    return dsc_transport_send(s->tr, json, strlen(json), err, errlen);
}

/* 组 hello 报文：t=hello，带本端设备标识、名称、本端码与已输入的对端码。 */
static int build_hello(dsc_session *s, const char *type, const char *entered, sb *out)
{
    char dev[40], name[64];
    dsc_device_id(s->db, dev, sizeof(dev));
    dsc_device_name(s->db, name, sizeof(name));
    jw w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "t", type);
    jw_put_str(&w, "device_id", dev);
    jw_put_str(&w, "name", name);
    jw_put_str(&w, "code", dsc_pairing_code(s->pair));
    jw_put_str(&w, "entered", entered ? entered : "");
    jw_put_int(&w, "schema", DSC_SYNC_SCHEMA_VERSION);
    jw_obj_end(&w);
    return DSC_OK;
}

/* 收一帧并解析为 jv；超时/断开原样返回。 */
static int recv_json(dsc_session *s, int timeout_ms, jv **out, char *err, size_t errlen)
{
    char *buf = NULL;
    size_t len = 0;
    int rc = dsc_transport_recv(s->tr, &buf, &len, timeout_ms, err, errlen);
    if (rc != DSC_OK) return rc;
    char jerr[256];
    jerr[0] = '\0';
    rc = jv_parse(buf, len, out, jerr, sizeof(jerr));
    dsc_xfree(buf);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "对端报文不是合法 JSON：%s", jerr);
        return DSC_E_FORMAT;
    }
    return DSC_OK;
}

/* 双方用同一份校验逻辑：本端输入的对端码 vs 对端 hello 里的 code；
 * 对端输入的本端码 vs 本端码。 */
static int verify_exchange(dsc_session *s, const jv *peer_hello, const char *our_entered_peer_code,
                           char *err, size_t errlen)
{
    const char *pcode = jv_get_str(peer_hello, "code", "");
    const char *pdev = jv_get_str(peer_hello, "device_id", "");
    const char *pname = jv_get_str(peer_hello, "name", "");
    const char *p_entered_ours = jv_get_str(peer_hello, "entered", "");
    if (!pcode || !*pcode) {
        if (err && errlen) snprintf(err, errlen, "对端没有提供配对码");
        return DSC_E_FORMAT;
    }
    if (!our_entered_peer_code || strcmp(our_entered_peer_code, pcode) != 0) {
        if (err && errlen) snprintf(err, errlen, "配对码不正确：输入值与对端显示的值不一致");
        return DSC_E_AUTH;
    }
    dsc_pair_record rec;
    int rc = dsc_pairing_verify(s->pair, pdev, pname, pcode, p_entered_ours, dsc_now_ms(),
                                &rec, err, errlen);
    if (rc != DSC_OK) return rc;
    dsc_strlcpy_utf8(s->peer_id, sizeof(s->peer_id), rec.peer_id);
    dsc_strlcpy_utf8(s->peer_name, sizeof(s->peer_name), rec.peer_name);
    rc = dsc_sync_set_realtime(s->db, 1); /* 配对成功即开启实时同步（FR-28） */
    if (rc != DSC_OK) return rc;
    rc = phase_to(s, DSC_SYNC_PAIRED, err, errlen);
    if (rc != DSC_OK) return rc;
    return phase_to(s, DSC_SYNC_SYNCING, err, errlen);
}

int dsc_session_handshake_connect(dsc_session *s, const char *peer_code, int timeout_ms,
                                  char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s || !s->tr) return DSC_E_STATE;
    if (s->phase != DSC_SYNC_PAIRING && s->phase != DSC_SYNC_CONNECTING) {
        if (err && errlen) snprintf(err, errlen, "当前状态 %s 不能进行配对握手",
                                    dsc_sync_state_name(s->phase));
        return DSC_E_STATE;
    }
    sb hs;
    sb_init(&hs);
    build_hello(s, "hello", NULL, &hs);
    /* 连接方先发 hello（不含 entered：对端还没输入），等对端回 hello_ack 后再发确认。 */
    int rc = send_json(s, sb_cstr(&hs), err, errlen);
    sb_free(&hs);
    if (rc != DSC_OK) return rc;

    jv *ack = NULL;
    rc = recv_json(s, timeout_ms, &ack, err, errlen);
    if (rc != DSC_OK) return rc;
    const char *t = jv_get_str(ack, "t", "");
    if (strcmp(t, "hello_ack") != 0) {
        jv_free(ack);
        if (err && errlen) snprintf(err, errlen, "对端报文类型异常：%s", t);
        return DSC_E_FORMAT;
    }
    /* 把用户输入的对端码作为 entered 交给对端校验：先补发一次带 entered 的 hello。 */
    sb hs2;
    sb_init(&hs2);
    build_hello(s, "hello", peer_code, &hs2);
    rc = send_json(s, sb_cstr(&hs2), err, errlen);
    sb_free(&hs2);
    if (rc == DSC_OK) rc = verify_exchange(s, ack, jv_get_str(ack, "code", ""), err, errlen);
    jv_free(ack);
    if (rc != DSC_OK) {
        send_json(s, "{\"t\":\"pair_fail\"}", NULL, 0);
        return rc;
    }
    rc = send_json(s, "{\"t\":\"pair_ok\"}", err, errlen);
    return rc;
}

int dsc_session_handshake_accept(dsc_session *s, const char *peer_code, int timeout_ms,
                                 char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s || !s->tr) return DSC_E_STATE;
    if (s->phase != DSC_SYNC_PAIRING && s->phase != DSC_SYNC_CONNECTING) {
        if (err && errlen) snprintf(err, errlen, "当前状态 %s 不能进行配对握手",
                                    dsc_sync_state_name(s->phase));
        return DSC_E_STATE;
    }
    /* 1) 收连接方的 hello（此时 entered 还是空的）。 */
    jv *hello = NULL;
    int rc = recv_json(s, timeout_ms, &hello, err, errlen);
    if (rc != DSC_OK) return rc;
    const char *t = jv_get_str(hello, "t", "");
    if (strcmp(t, "hello") != 0) {
        jv_free(hello);
        if (err && errlen) snprintf(err, errlen, "对端报文类型异常：%s", t);
        return DSC_E_FORMAT;
    }
    /* 2) 先按 §16.2 校验本端输入的对端码是否等于对端生成的码。 */
    const char *pcode = jv_get_str(hello, "code", "");
    if (!pcode || !*pcode) {
        jv_free(hello);
        if (err && errlen) snprintf(err, errlen, "对端没有提供配对码");
        return DSC_E_FORMAT;
    }
    if (!peer_code || strcmp(peer_code, pcode) != 0) {
        jv_free(hello);
        send_json(s, "{\"t\":\"pair_fail\"}", NULL, 0);
        if (err && errlen) snprintf(err, errlen, "配对码不正确：输入值与对端显示的值不一致");
        return DSC_E_AUTH;
    }
    /* 3) 回 hello_ack：本端码 + 本端已输入的对端码，供对端做同样的双向校验。 */
    sb ack;
    sb_init(&ack);
    build_hello(s, "hello_ack", peer_code, &ack);
    rc = send_json(s, sb_cstr(&ack), err, errlen);
    sb_free(&ack);
    jv_free(hello);
    if (rc != DSC_OK) return rc;

    /* 4) 收连接方补发、带 entered 的 hello。 */
    jv *hello2 = NULL;
    rc = recv_json(s, timeout_ms, &hello2, err, errlen);
    if (rc != DSC_OK) return rc;
    t = jv_get_str(hello2, "t", "");
    if (strcmp(t, "hello") != 0) {
        jv_free(hello2);
        send_json(s, "{\"t\":\"pair_fail\"}", NULL, 0);
        if (err && errlen) snprintf(err, errlen, "对端报文类型异常：%s", t);
        return DSC_E_FORMAT;
    }
    /* 5) 双向校验并建立配对关系（entered 由 dsc_pairing_verify 内部与本端码比对）。 */
    rc = verify_exchange(s, hello2, peer_code, err, errlen);
    jv_free(hello2);
    if (rc != DSC_OK) {
        send_json(s, "{\"t\":\"pair_fail\"}", NULL, 0);
        return rc;
    }
    /* 6) 等连接方的 pair_ok（可能已经并入下一次 pump，这里超时不算失败）。 */
    jv *done = NULL;
    char rerr[256];
    rerr[0] = '\0';
    if (recv_json(s, timeout_ms, &done, rerr, sizeof(rerr)) == DSC_OK) {
        t = jv_get_str(done, "t", "");
        jv_free(done);
        if (strcmp(t, "pair_fail") == 0) {
            if (err && errlen) snprintf(err, errlen, "对端配对校验未通过，本次配对已终止");
            dsc_pairing_unpair(s->db, s->peer_id, NULL, 0);
            s->peer_id[0] = '\0';
            s->peer_name[0] = '\0';
            return DSC_E_AUTH;
        }
    }
    return DSC_OK;
}

int dsc_session_publish(dsc_session *s, dsc_sync_entity entity, dsc_sync_op op,
                        const char *entity_id, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return DSC_E_INVAL;
    char eid[DSC_SYNC_ID_MAX];
    return dsc_sync_capture(s->sync, entity, op, entity_id, eid, sizeof(eid), err, errlen);
}

static int flush_outbox(dsc_session *s, int *out_sent, char *err, size_t errlen)
{
    int sent = 0;
    if (!dsc_sync_realtime_enabled(s->db)) {
        if (out_sent) *out_sent = 0;
        return DSC_OK; /* 已暂停：只停新事件发送，队列保留 */
    }
    for (int i = 0; i < DSC_SESSION_PENDING_MAX; i++) {
        if (s->pending_n >= DSC_SESSION_PENDING_MAX) break;
        dsc_sync_event ev;
        int rc = dsc_sync_outbox_next(s->sync, &ev);
        if (rc == DSC_E_NOTFOUND) break;
        if (rc != DSC_OK) return rc;
        sb frame;
        sb_init(&frame);
        sb_append(&frame, "{\"t\":\"event\",\"event\":");
        dsc_sync_event_to_json(&ev, &frame);
        sb_append(&frame, "}");
        rc = send_json(s, sb_cstr(&frame), err, errlen);
        sb_free(&frame);
        if (rc != DSC_OK) {
            dsc_sync_event_free(&ev);
            return rc; /* 链路断：事件仍在发件箱，恢复后续传 */
        }
        /* 记下已发出但未被对端确认的事件 id，收到 ack 才标记已发送（§16.3）。 */
        dsc_strlcpy_utf8(s->pending[s->pending_n], DSC_SYNC_ID_MAX, ev.event_id);
        s->pending_n++;
        dsc_sync_event_free(&ev);
        sent++;
    }
    if (out_sent) *out_sent = sent;
    return DSC_OK;
}

int dsc_session_pump(dsc_session *s, int timeout_ms, int *out_sent, int *out_applied,
                     char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (out_sent) *out_sent = 0;
    if (out_applied) *out_applied = 0;
    if (!s || !s->tr) return DSC_E_STATE;
    if (s->phase != DSC_SYNC_SYNCING) return DSC_E_STATE;

    char send_err[256];
    send_err[0] = '\0';
    int sent = 0;
    int rc = flush_outbox(s, &sent, send_err, sizeof(send_err));
    if (out_sent) *out_sent = sent;
    if (rc != DSC_OK) {
        /* 链路断：进入 PAIRED，事件留在队列等恢复。 */
        char e2[256];
        e2[0] = '\0';
        phase_to(s, DSC_SYNC_PAIRED, e2, sizeof(e2));
        if (err && errlen) snprintf(err, errlen, "同步链路中断：%s", send_err[0] ? send_err : dsc_strerror(rc));
        return rc;
    }

    int applied = 0;
    for (int round = 0; round < 16; round++) {
        jv *msg = NULL;
        char rerr[256];
        rerr[0] = '\0';
        rc = recv_json(s, round == 0 ? timeout_ms : 0, &msg, rerr, sizeof(rerr));
        if (rc == DSC_E_TIMEOUT) break;
        if (rc != DSC_OK) {
            char e2[256];
            e2[0] = '\0';
            phase_to(s, DSC_SYNC_PAIRED, e2, sizeof(e2));
            if (err && errlen) snprintf(err, errlen, "同步链路中断：%s", rerr[0] ? rerr : dsc_strerror(rc));
            return rc;
        }
        const char *t = jv_get_str(msg, "t", "");
        if (strcmp(t, "event") == 0) {
            const jv *evj = jv_get(msg, "event");
            if (evj) {
                sb evtxt;
                sb_init(&evtxt);
                jv_serialize(evj, &evtxt);
                int did = 0;
                int arc = dsc_sync_apply_json(s->sync, sb_cstr(&evtxt), strlen(sb_cstr(&evtxt)), &did,
                                              rerr, sizeof(rerr));
                sb_free(&evtxt);
                if (arc == DSC_OK) {
                    if (did) applied++;
                    /* 回 ack，对端据此把事件标记为已发送。 */
                    const char *eid = jv_get_str(evj, "event_id", "");
                    char ack[128];
                    snprintf(ack, sizeof(ack), "{\"t\":\"ack\",\"event_id\":\"%s\"}", eid);
                    send_json(s, ack, NULL, 0);
                } else if (arc != DSC_E_DISABLED) {
                    if (err && errlen) snprintf(err, errlen, "应用同步事件失败：%s", rerr[0] ? rerr : dsc_strerror(arc));
                    jv_free(msg);
                    return arc;
                }
            }
        } else if (strcmp(t, "ack") == 0) {
            const char *eid = jv_get_str(msg, "event_id", "");
            for (int i = 0; i < s->pending_n; i++) {
                if (strcmp(s->pending[i], eid) == 0) {
                    dsc_sync_outbox_mark_sent(s->sync, eid);
                    /* 确认过的从待确认表里摘掉，后面的前移（pending_n 是计数，必须一起缩）。 */
                    for (int k = i; k + 1 < s->pending_n; k++) {
                        dsc_strlcpy_utf8(s->pending[k], DSC_SYNC_ID_MAX, s->pending[k + 1]);
                    }
                    s->pending_n--;
                    s->pending[s->pending_n][0] = '\0';
                    break;
                }
            }
        }
        jv_free(msg);
        if (applied && round >= 7) break; /* 分批处理，避免长时间占用界面线程 */
    }
    s->sent_total += sent;
    s->applied_total += applied;
    if (out_applied) *out_applied = applied;
    return DSC_OK;
}

int dsc_session_unpair(dsc_session *s, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!s) return DSC_E_INVAL;
    int rc = DSC_OK;
    if (s->peer_id[0]) rc = dsc_pairing_unpair(s->db, s->peer_id, err, errlen);
    dsc_session_close(s);
    s->peer_id[0] = '\0';
    s->peer_name[0] = '\0';
    return rc;
}
