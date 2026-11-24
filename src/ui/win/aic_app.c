/* aic_app.c —— Windows 桌面端界面（Win32 + GDI，纯 C，不引入脚本解释器）。
 *
 * 设计约束（技术设计 §3/§7）：
 *   - 界面层用平台原生能力，自绘全部控件，配色只来自 theme.h 的令牌（禁止硬编码色值）；
 *   - 三栏「我 / 设置 / 聊天」同级不入栈，AI 信息与设置子页入栈，浮层不销毁底层页面；
 *   - 动效只用位置与透明度，时长取自 theme.h 的 aic_motion（220/180/200/160/140/120 毫秒）。
 *
 * 本机没有可用的真实网络通道（产品裁决：模型层只留配置项与接口），
 * 因此聊天发送走「离线演示传输」：不发任何 HTTP 请求，只在本地流式吐出一段固定回复。
 */
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aic_types.h"
#include "avatar.h"
#include "backup.h"
#include "capability.h"
#include "diaglog.h"
#include "error_store.h"
#include "knowledge.h"
#include "memory.h"
#include "http.h"
#include "notify.h"
#include "pairing.h"
#include "sync.h"
#include "sync_clock.h"
#include "sync_session.h"
#include "transport.h"
#include "credential.h"
#include "db.h"
#include "engine.h"
#include "json.h"
#include "llm.h"
#include "perf.h"
#include "plugin.h"
#include "prompt.h"
#include "router.h"
#include "store_conversation.h"
#include "store_guide.h"
#include "store_message.h"
#include "store_persona.h"
#include "store_profile.h"
#include "store_setting.h"
#include "store_usage.h"
#include "theme.h"
#include "transfer.h"
#include "update.h"
#include "util.h"

#define APP_TITLE        "Sai"
#define APP_CLASS        L"SaiMainWindow"
#define NAV_W            96
#define PAD              16
#define LINE_H           26
#define INPUT_H          96
/* 技术设计 13.3：消息分页，一页 100 条，向上按需加载。 */
#define MSG_PAGE_SIZE    100
#define ANIM_TIMER_ID    1
#define ANIM_FRAME_MS    16

/* 命中区域的动作编号。 */
enum {
    ACT_NONE = 0,
    ACT_NAV_ME = 100, ACT_NAV_SETTINGS, ACT_NAV_CHAT,
    ACT_MODE_SWITCH,
    ACT_SEND, ACT_STOP, ACT_REGENERATE, ACT_AI_INFO, ACT_GUIDE_VIEW, ACT_PROFILE_EDIT,
    ACT_ROW_THEME, ACT_ROW_MODEL, ACT_ROW_KEY, ACT_ROW_EXPORT, ACT_ROW_IMPORT,
    ACT_ROW_PLUGIN, ACT_ROW_BALANCE, ACT_ROW_UPDATE, ACT_ROW_GUIDE, ACT_ROW_DATA,
    ACT_ROW_ABOUT, ACT_ROW_MOTION,
    ACT_OVERLAY_CLOSE, ACT_FIELD_SAVE, ACT_FIELD_CANCEL,
    ACT_QA_NEW, ACT_QA_SEARCH, ACT_QA_BACK, ACT_QA_DELETE,
    ACT_CONFIRM_YES, ACT_CONFIRM_NO,
    ACT_GUIDE_RESET, ACT_GUIDE_ITEM_BASE = 300,
    ACT_QA_THREAD_BASE = 400,
    ACT_MSG_BASE = 600,
    ACT_EXPORT_JSON, ACT_EXPORT_MD, ACT_EXPORT_TXT,
    ACT_UPDATE_TEST, ACT_UPDATE_SKIP, ACT_UPDATE_NOW, ACT_UPDATE_LATER,
    ACT_BACKUP_NOW, ACT_BACKUP_RESTORE, ACT_BACKUP_CLEAN, ACT_AVATAR_PICK,
    ACT_PLUGIN_IMPORT_ACT, ACT_PLUGIN_PANEL_CALL, ACT_PANEL_BASE = 1100, ACT_PLUGIN_TOGGLE_BASE = 1200, ACT_PLUGIN_UPDATE_BASE = 1400,
    ACT_MODEL_FLASH, ACT_MODEL_PRO,
    ACT_MSG_RECALL, ACT_MSG_EDIT, ACT_MSG_SAVE, ACT_MSG_COPY,
    ACT_LOAD_OLDER, ACT_PLUGIN_INSTALL_CONFIRM,
    ACT_UNDO, ACT_QA_RENAME, ACT_ROW_ADVANCED, ACT_AI_AVATAR, ACT_AI_MODEL_FLASH, ACT_AI_MODEL_PRO, ACT_AI_VERSIONS,
    ACT_PERSONA_ROLLBACK_BASE = 2200,
    ACT_COPY_BASE = 2600,
    ACT_SCOPE_CUR, ACT_SCOPE_ALL, ACT_SCOPE_RANGE, ACT_SCOPE_AVATAR,
    /* 双端配对与实时同步（技术设计第 16 章 FR-25 ~ FR-30） */
    ACT_ROW_SYNC, ACT_ROW_PAIR,
    ACT_CRED_KEY_EDIT, ACT_CRED_SAVE, ACT_CRED_DEMO, ACT_MODE_CHAT, ACT_MODE_QA,
    ACT_PAIR_BACK, ACT_PAIR_ADDR_EDIT, ACT_PAIR_CODE_EDIT, ACT_PAIR_HOST, ACT_PAIR_DISCOVER,
    ACT_PAIR_CONNECT, ACT_PAIR_HANDSHAKE, ACT_PAIR_TOGGLE, ACT_PAIR_CONFLICTS, ACT_PAIR_UNPAIR,
    ACT_PAIR_CH_LAN, ACT_PAIR_CH_BT,
    /* 内置知识库（FR-31 / FR-32）、错误中心（FR-34）、设备权限（FR-35） */
    ACT_ROW_KB, ACT_ROW_ERROR, ACT_ROW_CAP,
    ACT_KB_BACK, ACT_KB_CITE, ACT_KB_BOOKMARK, ACT_KB_SEARCH_EDIT, ACT_KB_SEARCH_GO,
    ACT_KB_CITE_CLEAR,
    ACT_ERR_BACK, ACT_ERR_ACK, ACT_ERR_CLEAR, ACT_ERR_EXPORT,
    ACT_CAP_BACK, ACT_CAP_ASK, ACT_CAP_PICK_MEDIA, ACT_CAP_PICK_FILES, ACT_CAP_PICK_APPS,
    /* 列表项与按钮编号区段（避开 ACT_COPY_BASE=2600 到 6696 的复制按钮段） */
    ACT_KB_ITEM_BASE = 7000,     /* +0..63 篇 */
    ACT_KB_PARA_BASE = 7100,     /* +0..15 段 */
    ACT_CAP_TOGGLE_BASE = 7200,  /* +0..5 项能力 */
    ACT_ERR_CAT_BASE = 7300,     /* +0..8 个类别筛选 */
    ACT_ERR_ACT_BASE = 7330,     /* +0..199 错误条目的动作按钮 */
    ACT_CAP_APP_BASE = 7400,     /* +0..39 已安装软件列表项 */
    ACT_FIELD_BASE = 900
};

typedef struct Region { RECT rc; int id; } Region;

typedef struct Field {
    char label[64];
    char buf[512];
    int  multiline;
    int  id;
} Field;

typedef struct App {
    HINSTANCE hinst;
    HWND      hwnd;
    aic_db       *db;
    aic_engine   *eng;
    aic_router   *router;
    char root[512];

    HFONT font_ui, font_title, font_small, font_bold, font_mono;
    int   dpi;

    /* 命中区域 */
    Region regions[320];
    int    region_n;

    /* 动效 */
    int    anim_on;
    int    anim_kind;
    long long anim_start;
    int    anim_dur;

    /* 每栏滚动位置 */
    int scroll[3];

    /* 聊天数据 */
    aic_message *msgs;
    size_t msg_n;
    char conv_id[40];
    char conv_title[128];
    char persona_name[64];
    char draft[4096];
    int  draft_len;
    int  input_focus;
    long long caret_tick;
    int  caret_on;

    /* 浮层 */
    aic_overlay_kind overlay;
    Field fields[8];
    int   field_n;
    int   field_focus;
    char  overlay_title[128];
    char  overlay_text[2048];
    int   confirm_action;

    /* 问答模式 */
    aic_conversation *threads;
    size_t thread_n;
    char   search[128];
    int    search_focus;

    /* 状态栏与提示 */
    char  status[512];
    long long status_until;
    char  toast[256];
    long long toast_until;

    int   quit_confirm;
    char  sel_msg[40];       /* 当前选中的消息（撤回/修改用） */
    int   pair_channel;      /* 配对通道：0=局域网 1=蓝牙 */

    /* 后台发送（技术设计 4.2：网络与流式解析跑工作线程，结果经消息队列回界面线程） */
    volatile LONG sending;
    HANDLE     worker;
    char       stream_text[65536];
    size_t     stream_len;
    int        offline_demo;

    /* 系统主题与通知 */
    int   system_night;
    int   notified_update;

    /* 更新 */
    aic_update_manifest upd;
    int        upd_available;
    char       upd_pkg[1024];
    long long  upd_last_check;

    /* 插件面板（ui.panel 扩展点返回的按钮清单） */
    char  plugin_panel[4096];
    int   plugin_panel_n;
    aic_plugin *plugs;
    size_t      plug_n;

    /* 余额（FR-11：账户接口 + 本地账本并列） */
    aic_llm_balance bal;
    int             bal_valid;
    long long       bal_ms;

    /* 撤回/修改后的短时撤销入口（技术设计 9.2） */
    char      undo_msg[40];
    int       undo_kind;        /* 1=撤回 2=修改 */
    int       undo_was_recalled;
    char      undo_prev[4096];
    long long undo_until;

    /* 导出范围（技术设计 8.1） */
    int  export_kind;           /* 0=JSON 1=Markdown 2=纯文本 */
    int  export_scope;          /* 0=当前会话 1=按模式全部 2=时间区间 */
    int  export_avatars;

    /* 消息分页（技术设计 13.3：分页加载，默认一页 100 条） */
    int  msg_page;
    int  msg_has_more;
    long long msg_oldest_seq;

    /* 插件安装前确认（技术设计 10.2：先展示来源、版本、权限） */
    char plugin_pending[1024];
    int  plugin_pending_overwrite;
    /* 表单未保存改动（技术设计 6.4：改动未保存时关闭要先确认） */
    int  form_dirty;

    /* 进阶指南页（技术设计 10.3 / 12.3） */
    int  page_advanced;

    /* 提示词版本列表（技术设计 7.2：可回退） */
    int  ver_n;
    int  ver_num[8];
    char ver_time[8][32];
    char ver_persona[40];

    /* 首次启动的 API Key 配置（FR-30：凭证未写入安全存储前不进入主页） */
    char cred_key[256];
    int  cred_focus;

    /* 双端配对与实时同步（第 16 章 FR-25 ~ FR-29） */
    aic_session *ses;
    volatile LONG sync_stop;
    HANDLE        sync_thread;
    int   page_pair;
    char  pair_code_in[16];
    char  pair_addr_in[64];
    int   pair_focus;          /* 0 无 / 1 对端码 / 2 地址 */
    aic_peer peers[8];
    int   peer_n;
    char  pair_msg[512];
    volatile LONG pair_busy;
    int   pair_role;           /* 1=被连接方 3=连接方（重试握手时用） */

    /* 内置知识库（FR-31 / FR-32）：章节树、全文检索、书签、引用片段 */
    aic_kb *kb;
    int  page_kb;              /* 1=知识库页（列表或文章） */
    char kb_article[64];       /* 非空时显示文章详情 */
    int  kb_para;              /* 选中的正文段落，-1 表示没有选中 */
    char kb_query[64];
    int  kb_focus;             /* 1=检索框聚焦 */
    char kb_hits[16][64];
    int  kb_hit_n;
    int  kb_only_bookmark;     /* 1=只看书签 */

    /* 错误中心（FR-34）：类别筛选与列表缓冲 */
    int  page_error;
    int  err_filter;           /* 0=全部，1..AIC_ERR_OTHER 为类别序号 */
    aic_error_entry err_rows[64];
    int  err_n;

    /* 设备权限（FR-35） */
    int  page_cap;
    char cap_msg[256];
    char app_list[40][128];    /* 已安装软件候选（只给用户挑一条） */
    int  app_n;
    int  editing_ai;           /* 正在改的是 AI 发言：保存走记忆重建（FR-36） */
} App;

static App g_app;

/* ---------- prototypes ---------- */
static void app_init(App *a);
static void app_shutdown(App *a);
static int  app_root_dir(App *a);

/* 诊断模式：命令行带 --diag 时把初始化过程写到 exe 同目录的 aic_diag.log。 */
static void w_to_u8(const wchar_t *s, char *out, int cap);
static int g_diag = 0;
static FILE *g_logf = NULL;
static void DLOG(const char *fmt, ...)
{
    char path[600];
    wchar_t wmod[600];
    char mod[1200];
    va_list ap;
    if (!g_diag) return;
    if (!g_logf) {
        DWORD n = GetModuleFileNameW(NULL, wmod, (DWORD)(sizeof(wmod) / sizeof(wmod[0])));
        if (n == 0 || n >= (DWORD)(sizeof(wmod) / sizeof(wmod[0]))) return;
        w_to_u8(wmod, mod, sizeof(mod));
        {
            char dir[480];
            aic_strlcpy_utf8(dir, sizeof(dir), mod);
            {
                char *slash = strrchr(dir, '\\');
                if (slash) slash[0] = 0;
            }
            snprintf(path, sizeof(path), "%s\\sai_diag.log", dir);
        }
        g_logf = fopen(path, "wb");
        if (!g_logf) return;
    }
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}
static int  app_open_db(App *a);
static void app_setup_engine(App *a);
static void app_load_chat(App *a);
static void app_reload_threads(App *a);
static int  app_has_older(App *a, long long before_seq);
static void app_load_older(App *a);
static void app_plugin_import_confirm(App *a);
static void app_status(App *a, const char *fmt, ...);
static void app_toast(App *a, const char *fmt, ...);
static void app_set_overlay(App *a, aic_overlay_kind kind, const char *title);
static void app_close_overlay(App *a);
static void app_try_close(App *a);
static void app_begin_anim(App *a, int kind, int dur_ms);
static void app_invalidate(App *a);

static COLORREF tok(App *a, aic_token t);
static void u8_to_w(const char *s, wchar_t *out, int cap);
static void w_to_u8(const wchar_t *s, char *out, int cap);
static void draw_text(HDC dc, HFONT f, const char *utf8, RECT rc, COLORREF color, UINT fmt);
static void fill_round(HDC dc, RECT rc, int r, COLORREF color);
static void stroke_round(HDC dc, RECT rc, int r, COLORREF color, int w);
static void reg(RECT rc, int id);
static void hit_clear(void);
static int  hit_test(int x, int y);

static void paint(App *a, HDC dc, RECT client);
static void paint_chat(App *a, HDC dc, RECT content);
static void paint_advanced(App *a, HDC dc, RECT content);
static void paint_credential(App *a, HDC dc, RECT client);
static void paint_mode_select(App *a, HDC dc, RECT client);
static void paint_pair(App *a, HDC dc, RECT content);
static void paint_kb(App *a, HDC dc, RECT content);
static void paint_error_center(App *a, HDC dc, RECT content);
static void paint_capability(App *a, HDC dc, RECT content);
static void app_kb_refresh(App *a);
static void app_kb_open(App *a, const char *article_id);
static void app_rebuild_fragment(App *a);
static void app_err_reload(App *a);
static void app_cap_provider_install(App *a);
static int  cap_pick_action(const char *kind);
static int  cap_pick_path(App *a, const wchar_t *filter, const char *key, const wchar_t *title);
static void app_cred_save(App *a);
static void app_cred_demo(App *a);
static void app_mode_select(App *a, aic_mode m);
static void app_sync_start(App *a);
static void app_sync_stop(App *a);
static void app_pair_start(App *a, int kind);
static void app_pair_unpair(App *a);
static void paint_me(App *a, HDC dc, RECT content);
static void paint_settings(App *a, HDC dc, RECT content);
static void paint_qa(App *a, HDC dc, RECT content);
static void paint_overlay(App *a, HDC dc, RECT client);
static void paint_nav(App *a, HDC dc, RECT client);

static void on_click(App *a, int x, int y);
static void on_char(App *a, wchar_t ch);
static void on_key(App *a, int vk);
static void on_wheel(App *a, int delta);
/* ---------- 首次启动顺序（FR-30）与凭证写入 ---------- */
static void app_cred_save(App *a)
{
    char err[256];
    size_t n = strlen(a->cred_key);

    err[0] = '\0';
    if (n < 20 || strncmp(a->cred_key, "sk-", 3) != 0) {
        app_status(a, "API Key 格式不正确：应以 sk- 开头且长度不少于 20 个字符");
        return;
    }
    if (aic_cred_save(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, a->cred_key,
                      err, sizeof(err)) != AIC_OK) {
        app_status(a, "写入系统安全存储失败：%s", err[0] ? err : "未知错误");
        return;
    }
    aic_setting_set(a->db, AIC_SET_CRED_HAS_KEY, "1");
    aic_guide_mark(a->db, "api_key", 0);
    app_setup_engine(a);
    aic_secure_zero(a->cred_key, sizeof(a->cred_key));
    a->cred_focus = 0;
    aic_router_credential_done(a->router);
    app_status(a, "凭证已写入系统安全存储，接下来选择默认模式");
    app_invalidate(a);
}

static void app_cred_demo(App *a)
{
    /* 演示旁路：不写任何凭证，只放行首启顺序，界面与设置页都会提示演示模式。 */
    a->offline_demo = 1;
    aic_setting_set(a->db, AIC_SET_CRED_HAS_KEY, "1");
    app_setup_engine(a);
    a->cred_focus = 0;
    aic_router_credential_done(a->router);
    app_status(a, "已进入演示模式：不写凭证、不发任何网络请求");
    app_invalidate(a);
}

static void app_mode_select(App *a, aic_mode m)
{
    if (aic_router_mode_select(a->router, m) != AIC_OK) {
        app_status(a, "进入主页失败，请重试");
        return;
    }
    aic_guide_mark(a->db, "mode_intro", 0);
    app_reload_threads(a);
    app_load_chat(a);
    app_status(a, m == AIC_MODE_CHAT ? "已进入聊天模式" : "已进入问答模式");
    app_invalidate(a);
}

/* ---------- 双端同步：后台搬运与配对动作（§4.2 工作线程 + 消息队列） ---------- */
#define WM_APP_SYNC (WM_APP + 16)
#define WM_APP_PAIR (WM_APP + 17)

static DWORD WINAPI sync_worker(LPVOID lp)
{
    App *a = (App *)lp;
    while (!InterlockedCompareExchange(&a->sync_stop, 1, 1)) {
        if (a->ses && aic_session_is_paired(a->ses)) {
            int sent = 0, applied = 0;
            char err[256];
            err[0] = '\0';
            if (aic_session_pump(a->ses, 400, &sent, &applied, err, sizeof(err)) == AIC_OK) {
                if (sent || applied)
                    PostMessageA(a->hwnd, WM_APP_SYNC, (WPARAM)sent, (LPARAM)applied);
            } else {
                Sleep(600);
            }
        } else {
            Sleep(400);
        }
    }
    return 0;
}

/* 当前配对通道：0=局域网 1=蓝牙；以 session 里的选择为准，界面只在未建会话时给出默认。 */
static int app_pair_channel(App *a)
{
    return a ? a->pair_channel : 0;
}

static void app_pair_set_channel(App *a, int ch)
{
    if (!a) return;
    if (a->ses && aic_session_use_channel(a->ses, ch) != AIC_OK) {
        app_status(a, "链路已经建立，先解除配对或等当前连接结束后再换通道");
        return;
    }
    a->pair_channel = ch;
    app_status(a, ch == AIC_CH_BLUETOOTH
                      ? "改用蓝牙通道：地址填已配对设备的蓝牙地址（如 AA:BB:CC:DD:EE:FF），通道号默认 1"
                      : "改用局域网通道：地址填对端的局域网 IPv4 地址");
    a->peer_n = 0;
    app_invalidate(a);
}

static void app_sync_start(App *a)
{
    if (!a->ses && aic_session_create(a->db, &a->ses) != AIC_OK) {
        a->ses = NULL;
        return;
    }
    if (a->ses) aic_session_use_channel(a->ses, a->pair_channel);
    if (!a->sync_thread) {
        a->sync_stop = 0;
        a->sync_thread = CreateThread(NULL, 0, sync_worker, a, 0, NULL);
    }
}

static void app_sync_stop(App *a)
{
    if (a->sync_thread) {
        InterlockedExchange(&a->sync_stop, 1);
        WaitForSingleObject(a->sync_thread, 3000);
        CloseHandle(a->sync_thread);
        a->sync_thread = NULL;
    }
}

struct PairJob {
    App *a;
    int  kind;      /* 1 监听等待连入 / 2 发现同网设备 / 3 直连该地址 / 4 重试握手 */
    int  channel;   /* 0=局域网 1=蓝牙：端口对蓝牙是 RFCOMM 通道号 */
    char addr[64];
    char code[16];
};

/* 局域网用固定 TCP 端口；蓝牙用 RFCOMM 通道号（默认 1，可在地址框里写 地址:通道号）。 */
static int pair_port(int channel, const char *addr)
{
    if (channel != AIC_CH_BLUETOOTH) return AIC_TRANSPORT_DEFAULT_PORT;
    const char *colon = addr ? strrchr(addr, ':') : NULL;
    if (colon) {
        /* 只有最后一节不含冒号且长度为 1~2 位时才当作通道号（蓝牙地址本身含冒号）。 */
        const char *p = colon + 1;
        if (*p && strlen(p) <= 2 && !strchr(p, ':')) return aic_parse_int(p, 0);
    }
    return AIC_BT_CHANNEL_MIN;
}

/* 去掉地址尾部的 ":通道号"（蓝牙地址本身是 AA:BB:CC:DD:EE:FF 这种，只去掉纯数字尾段）。 */
static void pair_strip_channel(const char *in, char *out, size_t cap)
{
    const char *colon = in ? strrchr(in, ':') : NULL;
    if (colon && colon[1] && strlen(colon + 1) <= 2 && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
        size_t n = (size_t)(colon - in);
        if (n >= cap) n = cap - 1;
        memcpy(out, in, n);
        out[n] = '\0';
        return;
    }
    aic_strlcpy_utf8(out, cap, in ? in : "");
}

static DWORD WINAPI pair_worker(LPVOID lp)
{
    struct PairJob *j = (struct PairJob *)lp;
    App *a = j->a;
    char err[256];
    char host[64];
    int rc = AIC_OK;
    int port = pair_port(j->channel, j->addr);

    err[0] = '\0';
    pair_strip_channel(j->addr, host, sizeof(host));
    if (j->kind == 1) {
        rc = aic_session_host(a->ses, port, NULL, err, sizeof(err));
        if (rc == AIC_OK) rc = aic_session_accept(a->ses, 60000, err, sizeof(err));
        if (rc == AIC_OK) {
            a->pair_role = 1;
            rc = aic_session_handshake_accept(a->ses, j->code, 20000, err, sizeof(err));
        }
    } else if (j->kind == 2) {
        aic_peer found[8];
        size_t n = 0;
        if (j->channel == AIC_CH_BLUETOOTH) {
            /* 蓝牙不做广播发现：列系统已配对设备。 */
            rc = aic_transport_bluetooth_peers(found, 8, &n, err, sizeof(err));
        } else {
            rc = aic_session_discover(a->ses, port, 1500, found, 8, &n, err, sizeof(err));
        }
        if (rc == AIC_OK) {
            int i;
            a->peer_n = (int)(n > 8 ? 8 : n);
            for (i = 0; i < a->peer_n; i++) a->peers[i] = found[i];
        }
    } else if (j->kind == 3) {
        rc = aic_session_connect(a->ses, host, port, 4000, NULL, err, sizeof(err));
        if (rc == AIC_OK) {
            a->pair_role = 3;
            rc = aic_session_handshake_connect(a->ses, j->code, 20000, err, sizeof(err));
        }
    } else {
        if (a->pair_role == 1) rc = aic_session_handshake_accept(a->ses, j->code, 20000, err, sizeof(err));
        else rc = aic_session_handshake_connect(a->ses, j->code, 20000, err, sizeof(err));
    }

    if (rc == AIC_OK) {
        if (j->kind == 2) snprintf(a->pair_msg, sizeof(a->pair_msg),
                                   j->channel == AIC_CH_BLUETOOTH
                                       ? "找到 %d 台已配对蓝牙设备，点一行填入地址后再连接"
                                       : "发现 %d 台设备，点一行填入地址后再连接", a->peer_n);
        else snprintf(a->pair_msg, sizeof(a->pair_msg), "配对成功：已建立同步链路，实时同步默认开启");
    } else {
        snprintf(a->pair_msg, sizeof(a->pair_msg), "%s", err[0] ? err : aic_strerror(rc));
    }
    aic_xfree(j);
    PostMessageA(a->hwnd, WM_APP_PAIR, (WPARAM)rc, 0);
    return 0;
}

static void app_pair_start(App *a, int kind)
{
    struct PairJob *j;

    if (!a->ses) app_sync_start(a);
    if (!a->ses) { app_status(a, "同步会话初始化失败"); return; }
    if (InterlockedCompareExchange(&a->pair_busy, 1, 0) != 0) {
        app_status(a, "上一次配对动作还没结束，请稍候");
        return;
    }
    if (kind != 2 && !aic_pair_code_valid(a->pair_code_in)) {
        InterlockedExchange(&a->pair_busy, 0);
        app_status(a, "请先填对端界面显示的 6 位配对码");
        return;
    }
    if (kind == 3 && !a->pair_addr_in[0]) {
        InterlockedExchange(&a->pair_busy, 0);
        app_status(a, a->pair_channel == AIC_CH_BLUETOOTH
                           ? "请先填对端的蓝牙地址（如 AA:BB:CC:DD:EE:FF），或先点「列出已配对蓝牙设备」"
                           : "请先填对端的局域网地址，或先点「发现同网设备」");
        return;
    }
    j = (struct PairJob *)aic_xcalloc(1, sizeof(*j));
    if (!j) { InterlockedExchange(&a->pair_busy, 0); return; }
    j->a = a;
    j->kind = kind;
    j->channel = a->pair_channel;
    aic_strlcpy_utf8(j->addr, sizeof(j->addr), a->pair_addr_in);
    aic_strlcpy_utf8(j->code, sizeof(j->code), a->pair_code_in);
    a->pair_msg[0] = '\0';
    if (!CreateThread(NULL, 0, pair_worker, j, 0, NULL)) {
        aic_xfree(j);
        InterlockedExchange(&a->pair_busy, 0);
        app_status(a, "起配对线程失败");
        return;
    }
    if (kind == 1) app_status(a, a->pair_channel == AIC_CH_BLUETOOTH
                                         ? "正在监听蓝牙 RFCOMM 通道，等待对端连入（最长 60 秒）"
                                         : "正在监听 %d 端口，等待对端连入（最长 60 秒）",
                              AIC_TRANSPORT_DEFAULT_PORT);
    else if (kind == 2) app_status(a, a->pair_channel == AIC_CH_BLUETOOTH
                                         ? "正在列出系统已配对的蓝牙设备…" : "正在广播发现同网设备…");
    else if (kind == 3) app_status(a, "正在连接对端…");
    else app_status(a, "正在重试握手…");
    app_invalidate(a);
}

static void app_pair_unpair(App *a)
{
    char err[256];
    aic_pair_record rec;

    err[0] = '\0';
    if (aic_pairing_current(a->db, &rec) != AIC_OK) {
        app_status(a, "当前没有配对关系");
        return;
    }
    if (a->ses && aic_session_unpair(a->ses, err, sizeof(err)) == AIC_OK) {
        a->pair_msg[0] = '\0';
        app_status(a, "已解除配对：共享密钥已删除，两端本地数据保留");
        app_invalidate(a);
        return;
    }
    if (aic_pairing_unpair(a->db, rec.peer_id, err, sizeof(err)) == AIC_OK) {
        app_status(a, "已解除配对：共享密钥已删除，两端本地数据保留");
    } else {
        app_status(a, "解除配对失败：%s", err[0] ? err : aic_strerror(AIC_E_STATE));
    }
    app_invalidate(a);
}

static void do_action(App *a, int act);static void app_copy_message(App *a);
static void app_copy_block(App *a, int act);
static void app_undo(App *a);
static void app_qa_rename(App *a);
static void app_ai_versions(App *a);
static void app_persona_rollback(App *a, int idx);
static void app_ai_avatar_pick(App *a);

/* ---------- 小工具 ---------- */
static void u8_to_w(const char *s, wchar_t *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!s || !*s) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0) out[0] = 0;
    else out[cap - 1] = 0;
}

static void w_to_u8(const wchar_t *s, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!s || !*s) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
    else out[cap - 1] = 0;
}

static COLORREF tok(App *a, aic_token t)
{
    /* 主题偏好：0 日间 / 1 夜间 / 2 跟随系统（a->system_night 来自系统个人化设置）。 */
    const aic_theme *th = NULL;
    char pref[16];
    if (aic_setting_get(a->db, AIC_SET_THEME, pref, sizeof(pref)) != AIC_OK) pref[0] = '\0';
    int p = 0;
    if (pref[0] == '1' || aic_str_ieq(pref, "night")) p = 1;
    else if (pref[0] == '2' || aic_str_ieq(pref, "system")) p = 2;
    th = aic_theme_resolve(p, a->system_night);
    unsigned int v = aic_theme_color(th, t);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void draw_text(HDC dc, HFONT f, const char *utf8, RECT rc, COLORREF color, UINT fmt)
{
    wchar_t w[2048];
    u8_to_w(utf8, w, 2048);
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, w, -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void fill_round(HDC dc, RECT rc, int r, COLORREF color)
{
    HBRUSH b = CreateSolidBrush(color);
    HPEN p = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void stroke_round(HDC dc, RECT rc, int r, COLORREF color, int w)
{
    HPEN p = CreatePen(PS_SOLID, w, color);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

static void reg(RECT rc, int id)
{
    if (g_app.region_n >= (int)(sizeof(g_app.regions) / sizeof(g_app.regions[0]))) return;
    g_app.regions[g_app.region_n].rc = rc;
    g_app.regions[g_app.region_n].id = id;
    g_app.region_n++;
}

static void hit_clear(void) { g_app.region_n = 0; }

static int hit_test(int x, int y)
{
    for (int i = g_app.region_n - 1; i >= 0; i--) {
        RECT r = g_app.regions[i].rc;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return g_app.regions[i].id;
    }
    return ACT_NONE;
}

static void app_invalidate(App *a) { if (a->hwnd) InvalidateRect(a->hwnd, NULL, FALSE); }

static void app_begin_anim(App *a, int kind, int dur_ms)
{
    if (aic_setting_get_int(a->db, AIC_SET_MOTION, 1) == 0) return;   /* 减少动态效果 */
    a->anim_on = 1;
    a->anim_kind = kind;
    a->anim_start = aic_now_ms();
    a->anim_dur = dur_ms > 0 ? dur_ms : 1;
    app_invalidate(a);
}

static void app_status(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);
    a->status_until = aic_now_ms() + 6000;
    app_invalidate(a);
}

static void app_toast(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->toast, sizeof(a->toast), fmt, ap);
    va_end(ap);
    a->toast_until = aic_now_ms() + 2200;
    app_invalidate(a);
}


/* ---------- 同步入队（§16.4：实体变更后把增量事件交给发件箱） ---------- */
/* 只有已配对才入队；未配对或链路断开时事件留在发件箱，恢复后按 event_id 续传。 */
static void app_publish(App *a, aic_sync_entity entity, aic_sync_op op, const char *entity_id)
{
    char err[256];
    int rc;

    if (!a || !a->db || !a->ses || !entity_id || !*entity_id) return;
    if (!aic_session_is_paired(a->ses)) return;
    err[0] = '\0';
    rc = aic_session_publish(a->ses, entity, op, entity_id, err, sizeof(err));
    if (rc == AIC_OK) {
        app_invalidate(a);
    } else if (rc != AIC_E_DISABLED) {
        DLOG("publish %s/%s %s 失败：%d %s", aic_sync_entity_name(entity),
             aic_sync_op_name(op), entity_id, rc, err);
    }
}

static void app_publish_setting(App *a, const char *key)
{
    app_publish(a, AIC_ENT_SETTING, AIC_OP_UPDATE, key);
}
/* ---------- 数据目录 / 数据库 / 引擎 ---------- */
static void demo_reply_preview(App *a, char *out, size_t cap)
{
    snprintf(out, cap,
             "我在的，%s。这是离线演示回复：本轮只提供模型接入的配置项与协议层，不发起真实网络请求。"
             "配置 API Key 并接入真实传输后，这里就会显示模型的流式输出。",
             a->persona_name[0] ? a->persona_name : "陪伴助手");
}

/* 离线演示传输：按 SSE 协议切片喂给解析器，不碰网络（产品裁决：不发真实 HTTP）。 */
static aic_llm_err demo_post(void *ud, const char *url, const char *api_key,
                             const char *body, aic_sse_parser *parser,
                             aic_llm_event_fn fn, void *cb_ud, volatile int *cancel,
                             char *err, size_t errlen)
{
    App *a = (App *)ud;
    (void)url; (void)body;
    if (!api_key || !api_key[0]) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "尚未配置 API Key，请先到设置里填写");
        return AIC_LLM_E_AUTH;
    }
    char reply[700];
    demo_reply_preview(a, reply, sizeof(reply));
    size_t total = strlen(reply);
    size_t start = 0;
    for (int i = 1; i <= 3; i++) {
        if (cancel && *cancel) return AIC_LLM_E_CANCEL;
        size_t end = total * (size_t)i / 3;
        if (end > total) end = total;
        char piece[400];
        size_t n = end - start;
        if (n >= sizeof(piece)) n = sizeof(piece) - 1;
        memcpy(piece, reply + start, n);
        piece[n] = '\0';
        start = end;

        sb chunk;
        sb_init(&chunk);
        sb_append(&chunk, "data: {\"choices\":[{\"delta\":{\"content\":");
        jw_escape_into(&chunk, piece);
        sb_append(&chunk, "}}]}\n\n");
        aic_sse_feed(parser, chunk.data, chunk.len, fn, cb_ud);
        sb_free(&chunk);
    }
    {
        sb tail;
        sb_init(&tail);
        sb_append(&tail, "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
                         "\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":");
        char num[32];
        snprintf(num, sizeof(num), "%d", (int)(aic_utf8_length(reply) * 2));
        sb_append(&tail, num);
        sb_append(&tail, "}}\n\ndata: [DONE]\n\n");
        aic_sse_feed(parser, tail.data, tail.len, fn, cb_ud);
        sb_free(&tail);
    }
    return AIC_LLM_OK;
}

/* 连通性校验：离线模式下只要 Key 非空即视为可用（明确说明不发请求）。 */
static aic_llm_err demo_probe(void *ud, const char *url, const char *api_key,
                              char *err, size_t errlen)
{
    (void)ud; (void)url;
    if (!api_key || !api_key[0]) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "API Key 为空");
        return AIC_LLM_E_AUTH;
    }
    return AIC_LLM_OK;
}

static int app_open_db(App *a)
{
    char dbdir[600], dbp[700], bkp[600];
    aic_path_join(dbdir, sizeof(dbdir), a->root, "db");
    aic_path_join(dbp, sizeof(dbp), dbdir, "aic.db");
    aic_path_join(bkp, sizeof(bkp), a->root, "backups");

    int rc = aic_db_open(dbp, &a->db);
    if (rc != AIC_OK) return rc;
    int from = 0, to = 0;
    rc = aic_db_migrate(a->db, dbp, bkp, &from, &to);
    if (rc != AIC_OK) return rc;
    return AIC_OK;
}

static void app_load_chat(App *a)
{
    if (!a->db) return;
    aic_conversation conv;
    memset(&conv, 0, sizeof(conv));
    int rc;
    if (aic_router_mode(a->router) == AIC_MODE_QA) {
        const char *tid = aic_router_thread(a->router);
        if (!tid || !tid[0]) {
            a->msg_n = 0;
            aic_xfree(a->msgs);
            a->msgs = NULL;
            a->conv_id[0] = '\0';
            return;
        }
        rc = aic_conv_get(a->db, tid, &conv);
    } else {
        char model[64];
        if (aic_setting_get(a->db, AIC_SET_MODEL_DEFAULT, model, sizeof(model)) != AIC_OK || !model[0])
            aic_strlcpy_utf8(model, sizeof(model), AIC_MODEL_FLASH);
        rc = aic_conv_ensure_chat(a->db, model, NULL, &conv);
    }
    if (rc != AIC_OK) {
        a->msg_n = 0;
        aic_xfree(a->msgs);
        a->msgs = NULL;
        a->conv_id[0] = '\0';
        return;
    }
    aic_strlcpy_utf8(a->conv_id, sizeof(a->conv_id), conv.id);
    aic_strlcpy_utf8(a->conv_title, sizeof(a->conv_title), conv.title);

    aic_xfree(a->msgs);
    a->msgs = NULL;
    a->msg_n = 0;
    a->msg_has_more = 0;
    a->msg_oldest_seq = 0;
    /* 技术设计 13.3：只加载最近一页，向上按需再翻，避免万条消息一次性进内存。 */
    if (aic_msg_page(a->db, conv.id, 0, MSG_PAGE_SIZE, &a->msgs, &a->msg_n) != AIC_OK) {
        a->msg_n = 0;
        return;
    }
    if (a->msg_n > 0) {
        a->msg_oldest_seq = a->msgs[0].seq;
        a->msg_has_more = app_has_older(a, a->msg_oldest_seq);
    }
    aic_persona per;
    memset(&per, 0, sizeof(per));
    if (aic_persona_ensure_default(a->db, &per) == AIC_OK) {
        aic_strlcpy_utf8(a->persona_name, sizeof(a->persona_name), per.name);
        aic_persona_free(&per);
    }
}

/* 是否还有更早的一页（技术设计 13.3）。 */
static int app_has_older(App *a, long long before_seq)
{
    aic_message *one = NULL;
    size_t n = 0;
    int rc;
    if (!a->db || !a->conv_id[0] || before_seq <= 1) return 0;
    rc = aic_msg_page(a->db, a->conv_id, before_seq, 1, &one, &n);
    if (one) aic_msg_free_array(one, n);
    return (rc == AIC_OK && n > 0) ? 1 : 0;
}

/* 向上翻一页：把更早的一页拼到当前列表前面（技术设计 13.3）。 */
static void app_load_older(App *a)
{
    aic_message *older = NULL;
    size_t on = 0;
    aic_message *all;
    if (!a->db || !a->msg_has_more || a->msg_oldest_seq <= 1) return;
    if (aic_msg_page(a->db, a->conv_id, a->msg_oldest_seq, MSG_PAGE_SIZE, &older, &on) != AIC_OK) return;
    if (on == 0) { a->msg_has_more = 0; return; }
    all = (aic_message *)aic_xcalloc(a->msg_n + on, sizeof(aic_message));
    memcpy(all, older, on * sizeof(aic_message));
    if (a->msgs && a->msg_n) memcpy(all + on, a->msgs, a->msg_n * sizeof(aic_message));
    aic_xfree(older);
    aic_xfree(a->msgs);
    a->msgs = all;
    a->msg_n += on;
    a->msg_oldest_seq = a->msgs[0].seq;
    a->msg_has_more = app_has_older(a, a->msg_oldest_seq);
    app_status(a, "已加载更早的 %d 条消息", (int)on);
}

static void app_reload_threads(App *a)
{
    aic_conversation *arr = NULL;
    size_t n = 0;
    int rc = a->search[0]
             ? aic_conv_search(a->db, AIC_MODE_QA, a->search, &arr, &n)
             : aic_conv_list(a->db, AIC_MODE_QA, &arr, &n);
    if (rc != AIC_OK) return;
    if (a->threads) aic_conv_free(a->threads, a->thread_n);
    a->threads = arr;
    a->thread_n = n;
}

static void md_strip(const char *in, char *out, size_t cap);
static void on_stream_delta(void *ud, const char *delta);
static void app_update(App *a);

/* ---------- 注入片段：插件扩展点 + 知识库引用片段（第 10 章 + §17 / FR-31 / FR-32） ----------
 * 选中的段落作为引用片段进插件注入区，共用同一份上下文预算；片段只带那一段与一行来源，不携全文。 */
static void app_rebuild_fragment(App *a)
{
    sb frag;
    char cite[2048];

    if (!a->eng) return;
    cite[0] = '\0';
    if (aic_setting_get(a->db, AIC_SET_KB_CITE, cite, sizeof(cite)) != AIC_OK) cite[0] = '\0';
    sb_init(&frag);
    if (aic_plugin_build_fragment(a->db, &frag, NULL, 0) != AIC_OK) sb_reset(&frag);
    if (cite[0]) {
        if (sb_cstr(&frag)[0]) sb_append(&frag, "\n\n");
        sb_append(&frag, cite);
    }
    aic_engine_set_plugin_fragment(a->eng, sb_cstr(&frag));
    sb_free(&frag);
}

/* ---------- 内置知识库（FR-31 / FR-32） ---------- */
static void app_kb_refresh(App *a)
{
    char err[256];
    if (a->kb) return;
    err[0] = '\0';
    if (aic_kb_load(&a->kb, err, sizeof(err)) != AIC_OK) {
        a->kb = NULL;
        app_status(a, "内置知识库载入失败：%s", err[0] ? err : "数据损坏");
    }
}

static void app_kb_open(App *a, const char *article_id)
{
    app_kb_refresh(a);
    if (!a->kb) return;
    if (!article_id || !article_id[0] || !aic_kb_find(a->kb, article_id)) {
        app_status(a, "知识库里没有这一篇");
        return;
    }
    aic_strlcpy_utf8(a->kb_article, sizeof(a->kb_article), article_id);
    a->kb_para = -1;
    a->scroll[1] = 0;
    aic_kb_recent_push(a->db, article_id);
}

/* ---------- 错误中心（FR-34 §9.4） ---------- */
static const char *g_err_cats[] = {
    AIC_ERR_NETWORK, AIC_ERR_QUOTA, AIC_ERR_RATE_LIMIT, AIC_ERR_PARSE,
    AIC_ERR_CREDENTIAL, AIC_ERR_CAPABILITY, AIC_ERR_STORAGE, AIC_ERR_OTHER
};

static int g_err_cat_n = (int)(sizeof(g_err_cats) / sizeof(g_err_cats[0]));

static void app_err_reload(App *a)
{
    aic_error_filter f;
    int cap = (int)(sizeof(a->err_rows) / sizeof(a->err_rows[0]));

    memset(&f, 0, sizeof(f));
    if (a->err_filter > 0 && a->err_filter <= g_err_cat_n) f.category = g_err_cats[a->err_filter - 1];
    f.limit = cap;
    memset(a->err_rows, 0, sizeof(a->err_rows));
    a->err_n = 0;
    if (aic_error_query(a->db, &f, a->err_rows, cap, &a->err_n) != AIC_OK) a->err_n = 0;
    if (a->err_n > cap) a->err_n = cap;
}

/* ---------- 设备能力（FR-35 §18）：平台取值函数 ----------
 * 桌面端只做真实可得的三项，并且都要用户先显式选中：
 *   - 设备图片：在设备权限页选一个图片文件，注入时只给名称与大小，原图不上传；
 *   - 已安装软件：从注册表卸载项里挑一条，只给这一个名字，不给完整清单；
 *   - 本地文件：选单个文件，注入前读文本片段，不递归、不传原始文件。
 * 位置与天气在桌面端没有可用来源：返回 AIC_E_STATE（能力已授权也不算错），
 * 由错误中心写一条 capability.unavailable，主对话继续。 */
static int cap_none(char *buf, size_t cap, void *ud)
{
    (void)ud;
    if (buf && cap) buf[0] = '\0';
    return AIC_E_STATE;
}

static int cap_media_text(char *buf, size_t cap, void *ud)
{
    char sel[600];
    int64_t size = 0;
    (void)ud;
    sel[0] = '\0';
    if (aic_setting_get(g_app.db, AIC_SET_CAP_PREFIX "media.selected", sel, sizeof(sel)) != AIC_OK || !sel[0])
        return AIC_E_STATE;
    if (aic_file_size(sel, &size) != AIC_OK) return AIC_E_STATE;
    snprintf(buf, cap, "用户选中的图片：%s（约 %lld KB；原始图片不上传，只记名称与大小）",
             aic_path_basename(sel), (long long)(size / 1024));
    return AIC_OK;
}

static int cap_apps_text(char *buf, size_t cap, void *ud)
{
    char sel[160];
    (void)ud;
    sel[0] = '\0';
    if (aic_setting_get(g_app.db, AIC_SET_CAP_PREFIX "apps.selected", sel, sizeof(sel)) != AIC_OK || !sel[0])
        return AIC_E_STATE;
    aic_strlcpy_utf8(buf, cap, sel);
    return AIC_OK;
}

static int cap_files_text(char *buf, size_t cap, void *ud)
{
    char sel[600];
    sb text;
    int rc;
    (void)ud;
    sel[0] = '\0';
    if (aic_setting_get(g_app.db, AIC_SET_CAP_PREFIX "files.selected", sel, sizeof(sel)) != AIC_OK || !sel[0])
        return AIC_E_STATE;
    sb_init(&text);
    rc = aic_cap_read_text_file(g_app.db, sel, &text);
    if (rc == AIC_OK) aic_strlcpy_utf8(buf, cap, sb_cstr(&text));
    sb_free(&text);
    return rc;
}

static void app_cap_provider_install(App *a)
{
    aic_cap_provider p;
    memset(&p, 0, sizeof(p));
    p.time_now = NULL;          /* 时间用本地时钟真实现（capability.c 内置，不依赖平台） */
    p.location = cap_none;
    p.weather = cap_none;
    p.media_pick = cap_media_text;
    p.apps_pick = cap_apps_text;
    p.files_pick = cap_files_text;
    p.ud = a;
    aic_cap_set_provider(&p);
}

/* 已安装软件：只读取卸载项里的 DisplayName，最多取 40 条给用户挑一条。 */
static int cap_enum_reg(char (*names)[128], int cap, HKEY root, const char *path)
{
    HKEY k;
    DWORD idx;
    int n = 0;

    if (RegOpenKeyExA(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    for (idx = 0; n < cap; idx++) {
        char sub[256];
        DWORD sublen = (DWORD)sizeof(sub);
        HKEY sk;
        char name[256];
        DWORD namelen = (DWORD)sizeof(name);
        DWORD type = 0;
        int dup = 0;
        if (RegEnumKeyExA(k, idx, sub, &sublen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExA(k, sub, 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;
        namelen = (DWORD)sizeof(name);
        if (RegQueryValueExA(sk, "DisplayName", NULL, &type, (LPBYTE)name, &namelen) == ERROR_SUCCESS &&
            type == REG_SZ && name[0]) {
            for (int i = 0; i < n; i++) if (aic_str_ieq(names[i], name)) { dup = 1; break; }
            if (!dup) { aic_strlcpy_utf8(names[n], 128, name); n++; }
        }
        RegCloseKey(sk);
    }
    RegCloseKey(k);
    return n;
}

static int app_enum_apps(App *a)
{
    int n = cap_enum_reg(a->app_list, (int)(sizeof(a->app_list) / sizeof(a->app_list[0])),
                         HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    n += cap_enum_reg(a->app_list + n,
                      (int)(sizeof(a->app_list) / sizeof(a->app_list[0])) - n,
                      HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    n += cap_enum_reg(a->app_list + n,
                      (int)(sizeof(a->app_list) / sizeof(a->app_list[0])) - n,
                      HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    a->app_n = n;
    return n;
}

static void app_setup_engine(App *a)
{
    if (!a->eng) {
        if (aic_engine_create(a->db, &a->eng) != AIC_OK) return;
    }
    if (a->offline_demo || !aic_http_available()) {
        aic_llm_transport tr;
        memset(&tr, 0, sizeof(tr));
        tr.ud = a;
        tr.post_stream = demo_post;
        tr.probe = demo_probe;
        aic_engine_set_transport(a->eng, tr);
        DLOG("transport=demo offline=%d http=%d", a->offline_demo, aic_http_available());
    } else {
        aic_engine_set_transport(a->eng, aic_win_http_transport());
        DLOG("transport=winhttp");
    }

    char url[AIC_LLM_URL_MAX];
    if (aic_setting_get(a->db, AIC_SET_CRED_ENDPOINT, url, sizeof(url)) != AIC_OK || !url[0])
        aic_strlcpy_utf8(url, sizeof(url), AIC_LLM_MODEL_DEFAULT_URL);
    aic_engine_set_endpoint(a->eng, url);

    char key[512];
    key[0] = '\0';
    char err[256];
    if (aic_cred_load(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, key, sizeof(key),
                      err, sizeof(err)) == AIC_OK) {
        aic_engine_set_api_key(a->eng, key);
    } else {
        aic_engine_set_api_key(a->eng, "");
    }
    aic_secure_zero(key, sizeof(key));

    char model[64];
    if (aic_setting_get(a->db, AIC_SET_MODEL_DEFAULT, model, sizeof(model)) != AIC_OK || !model[0])
        aic_strlcpy_utf8(model, sizeof(model), AIC_MODEL_FLASH);
    aic_engine_set_model_override(a->eng, model);
    aic_engine_set_context_messages(a->eng, aic_setting_get_int(a->db, AIC_SET_CONTEXT_N,
                                                               AIC_CONTEXT_MESSAGES_DEFAULT));
    aic_engine_set_stream_cb(a->eng, on_stream_delta, a);

    /* FR-34：错误中心明细写 <数据根>/errors；FR-36：记忆条目记来源端；
     * FR-35：能力取值按用途展示，平台函数在 app_cap_provider_install 里注入。 */
    aic_engine_set_data_root(a->eng, a->root);
    {
        char dev[40];
        if (aic_device_id(a->db, dev, sizeof(dev)) == AIC_OK)
            aic_engine_set_origin_device(a->eng, dev);
    }
    aic_engine_set_memory_enabled(a->eng, 1);
    aic_engine_set_capability_enabled(a->eng, 1);
    app_cap_provider_install(a);
    app_rebuild_fragment(a);
}


/* ---------- 表单 / 浮层 ---------- */
#define FORM_NONE      0
#define FORM_PERSONA   1
#define FORM_KEY       2
#define FORM_PROFILE   3
#define FORM_EXPORT    4
#define FORM_CONFIRM   5
#define FORM_UPDATE    6
#define FORM_MSG       7
#define FORM_EDITMSG   8
#define FORM_DATA      9
#define FORM_PLUGIN    10
#define FORM_RENAME    11
#define FORM_VERSIONS  12
#define FORM_PLUGIN_IMPORT 13
#define FORM_DISCARD   14
#define APP_VERSION    "1.4.0"
#define MSG_CAP        512
#define ROW_H          52

static void fld(App *a, const char *label, const char *buf, int multiline, int id)
{
    if (a->field_n >= (int)(sizeof(a->fields) / sizeof(a->fields[0]))) return;
    Field *fl = &a->fields[a->field_n];
    memset(fl, 0, sizeof(*fl));
    aic_strlcpy_utf8(fl->label, sizeof(fl->label), label ? label : "");
    if (buf) aic_strlcpy_utf8(fl->buf, sizeof(fl->buf), buf);
    fl->multiline = multiline ? 1 : 0;
    fl->id = id;
    a->field_n++;
}

/* 技术设计 6.4：改动未保存时要先确认，避免点错把输入丢掉。 */
static void app_try_close(App *a)
{
    if (a->overlay != AIC_OV_NONE && a->form_dirty) {
        app_set_overlay(a, AIC_OV_CONFIRM, "放弃未保存的修改？");
        a->confirm_action = FORM_DISCARD;
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "这个表单里有还没保存的修改，关闭后这些修改会丢弃。确认放弃吗？");
        app_invalidate(a);
        return;
    }
    app_close_overlay(a);
}

static void app_close_overlay(App *a)
{
    a->overlay = AIC_OV_NONE;
    a->field_n = 0;
    a->field_focus = -1;
    a->confirm_action = 0;
    a->quit_confirm = 0;
    a->overlay_text[0] = '\0';
    aic_router_close_overlay(a->router);
    app_invalidate(a);
}

static void app_set_overlay(App *a, aic_overlay_kind kind, const char *title)
{
    /* 打开新浮层视为一次新的编辑会话，脏标记从干净开始。 */
    a->form_dirty = 0;
    a->overlay = kind;
    a->field_n = 0;
    a->field_focus = -1;
    a->confirm_action = FORM_NONE;
    aic_strlcpy_utf8(a->overlay_title, sizeof(a->overlay_title), title ? title : "");
    a->overlay_text[0] = '\0';
    aic_router_open_overlay(a->router, kind);
    app_begin_anim(a, 3, aic_motion_default().overlay_ms);
}

/* ---------- 文字度量 ---------- */
static int measure_text(HDC dc, HFONT f, const char *utf8, int width, int *out_w)
{
    wchar_t *w = (wchar_t *)aic_xmalloc(sizeof(wchar_t) * 16384);
    RECT rc;
    u8_to_w(utf8 ? utf8 : "", w, 16384);
    rc.left = 0; rc.top = 0; rc.right = width; rc.bottom = 0;
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, w, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    aic_xfree(w);
    if (out_w) *out_w = rc.right - rc.left;
    return rc.bottom - rc.top;
}

/* ---------- 通用控件绘制 ---------- */
static void draw_pill(App *a, HDC dc, RECT rc, const char *label, int id, int primary)
{
    if (primary) {
        fill_round(dc, rc, 8, tok(a, AIC_TOK_PRIMARY));
        draw_text(dc, a->font_ui, label, rc, RGB(255, 255, 255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        stroke_round(dc, rc, 8, tok(a, AIC_TOK_DIVIDER), 1);
        draw_text(dc, a->font_ui, label, rc, tok(a, AIC_TOK_TEXT),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    reg(rc, id);
}

static void draw_row(App *a, HDC dc, RECT *y, const char *label, const char *value, int id)
{
    RECT rr = {y->left, y->top, y->right, y->top + ROW_H};
    RECT lr = {rr.left + 16, rr.top, rr.right - 200, rr.bottom};
    RECT vr = {rr.right - 220, rr.top, rr.right - 16, rr.bottom};
    RECT dl = {rr.left + 16, rr.bottom - 1, rr.right, rr.bottom};
    HBRUSH b;

    draw_text(dc, a->font_ui, label, lr, tok(a, AIC_TOK_TEXT),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (value && value[0])
        draw_text(dc, a->font_small, value, vr, tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    b = CreateSolidBrush(tok(a, AIC_TOK_DIVIDER));
    FillRect(dc, &dl, b);
    DeleteObject(b);
    if (id) reg(rr, id);
    y->top += ROW_H;
}

/* ---------- 左导航 ---------- */
static void paint_nav(App *a, HDC dc, RECT client)
{
    RECT nav = {client.left, client.top, client.left + NAV_W, client.bottom};
    RECT sep = {nav.right - 1, nav.top, nav.right, nav.bottom};
    HBRUSH b = CreateSolidBrush(tok(a, AIC_TOK_CARD_BG));
    int y = nav.top + PAD;
    int active_tab = (int)aic_router_tab(a->router);
    int is_chat_mode = aic_router_mode(a->router) == AIC_MODE_CHAT;

    FillRect(dc, &nav, b);
    DeleteObject(b);
    b = CreateSolidBrush(tok(a, AIC_TOK_DIVIDER));
    FillRect(dc, &sep, b);
    DeleteObject(b);

    for (int i = 0; i < (int)AIC_TAB_COUNT; i++) {
        RECT r = {nav.left + 8, y, nav.right - 8, y + 56};
        int on = is_chat_mode && active_tab == i;
        if (on) fill_round(dc, r, 10, tok(a, AIC_TOK_PAGE_BG));
        draw_text(dc, on ? a->font_bold : a->font_ui, aic_tab_name((aic_tab)i), r,
                  on ? tok(a, AIC_TOK_PRIMARY) : tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (aic_router_badge(a->router, (aic_tab)i) > 0) {
            RECT br = {r.right - 24, r.top + 8, r.right - 8, r.top + 24};
            fill_round(dc, br, 8, tok(a, AIC_TOK_ACCENT));
        }
        reg(r, ACT_NAV_ME + i);
        y += 64;
    }

    {
        RECT r = {nav.left + 8, nav.bottom - 76, nav.right - 8, nav.bottom - 24};
        draw_pill(a, dc, r, is_chat_mode ? "问答模式" : "聊天模式", ACT_MODE_SWITCH, 0);
        r.top = nav.bottom - 20;
        draw_text(dc, a->font_small, is_chat_mode ? "当前：聊天" : "当前：问答", r,
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
}

/* ---------- 聊天内容区（两种模式共用） ---------- */
/* ---------- Markdown 轻量渲染（技术设计 6.1：代码块独立样式 + 复制按钮） ---------- */
#define MD_BLK_MAX   16
#define MD_BLK_BYTES 8192

typedef struct MdBlk { int code; int len; char buf[MD_BLK_BYTES]; } MdBlk;

/* 绘制期单线程使用，静态复用避免每帧分配 128KB。 */
static MdBlk g_md[MD_BLK_MAX];

static int md_fence(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    return (len - i >= 3 && line[i] == '`' && line[i + 1] == '`' && line[i + 2] == '`');
}

/* 把正文切成「普通文本块」与「代码块」。非代码块里去掉 ** 与行内反引号。 */
static int md_split(const char *in, MdBlk *blks, int max_blk)
{
    int n = 0;
    int code = 0;
    size_t pos = 0, len;
    if (!in || max_blk <= 0) return 0;
    len = strlen(in);
    while (pos < len && n <= max_blk) {
        size_t end = pos, ll;
        const char *line;
        while (end < len && in[end] != '\n') end++;
        ll = end - pos;
        line = in + pos;
        if (md_fence(line, ll)) {
            code = !code;
            pos = (end < len) ? end + 1 : len;
            continue;
        }
        if (n == 0 || blks[n - 1].code != code) {
            if (n > 0 && blks[n - 1].len == 0) {
                blks[n - 1].code = code;
            } else {
                if (n >= max_blk) break;
                n++;
                blks[n - 1].code = code;
                blks[n - 1].len = 0;
                blks[n - 1].buf[0] = '\0';
            }
        }
        {
            MdBlk *b = &blks[n - 1];
            size_t i;
            for (i = 0; i < ll; i++) {
                char c = line[i];
                if (!code) {
                    if (c == '*' && i + 1 < ll && line[i + 1] == '*') { i++; continue; }
                    if (c == '`') continue;
                }
                if ((size_t)b->len + 1 < sizeof(b->buf)) b->buf[b->len++] = c;
            }
            if ((size_t)b->len + 1 < sizeof(b->buf)) b->buf[b->len++] = '\n';
            b->buf[b->len] = '\0';
        }
        pos = (end < len) ? end + 1 : len;
    }
    /* 去掉块尾空行与空块。 */
    {
        int m = 0, i;
        for (i = 0; i < n; i++) {
            while (blks[i].len > 0 && blks[i].buf[blks[i].len - 1] == '\n')
                blks[i].buf[--blks[i].len] = '\0';
            if (blks[i].len > 0) {
                if (m != i) blks[m] = blks[i];
                m++;
            }
        }
        n = m;
    }
    return n;
}

/* 一条消息（含代码块）的高度；out_w 取最宽块宽度。 */
static int msg_height(HDC dc, App *a, const char *txt, int wrap, int *out_w)
{
    int n = md_split(txt, g_md, MD_BLK_MAX);
    int h = 0, maxw = 0, i;
    for (i = 0; i < n; i++) {
        HFONT f = g_md[i].code ? a->font_mono : a->font_ui;
        int w = 0;
        int bh = measure_text(dc, f, g_md[i].buf, wrap, &w);
        if (g_md[i].code) bh += 18;
        h += bh;
        if (w > maxw) maxw = w;
    }
    if (h <= 0) h = measure_text(dc, a->font_ui, " ", wrap, &maxw);
    if (out_w) *out_w = maxw;
    return h + 20;
}

/* 画消息正文（含代码块面板与「复制」按钮），返回结束 y。 */
static int msg_draw(App *a, HDC dc, int top, int left, int right, const char *txt, int wrap,
                    int idx, COLORREF color)
{
    int n = md_split(txt, g_md, MD_BLK_MAX);
    int y = top, i;
    for (i = 0; i < n; i++) {
        if (g_md[i].code) {
            int bh = measure_text(dc, a->font_mono, g_md[i].buf, wrap, NULL) + 18;
            RECT panel = {left, y, right, y + bh};
            RECT tr;
            RECT cp;
            fill_round(dc, panel, 8, tok(a, AIC_TOK_PAGE_BG));
            stroke_round(dc, panel, 8, tok(a, AIC_TOK_DIVIDER), 1);
            tr.left = panel.left + 10;
            tr.right = panel.right - 10;
            tr.top = panel.top + 8;
            tr.bottom = panel.bottom - 8;
            draw_text(dc, a->font_mono, g_md[i].buf, tr,
                      a->system_night ? RGB(220, 224, 230) : RGB(40, 44, 52),
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
            cp.left = panel.right - 62;
            cp.right = panel.right - 10;
            cp.top = panel.top + 4;
            cp.bottom = panel.top + 26;
            draw_pill(a, dc, cp, "复制", ACT_COPY_BASE + idx * 8 + i, 0);
            y += bh;
        } else {
            int bh = measure_text(dc, a->font_ui, g_md[i].buf, wrap, NULL);
            RECT tr = {left, y, right, y + bh};
            draw_text(dc, a->font_ui, g_md[i].buf, tr, color, DT_LEFT | DT_TOP | DT_WORDBREAK);
            y += bh;
        }
    }
    return y;
}

/* 日期分隔文案：今天 / 昨天 / 具体日期（技术设计 6.3）。 */
static void date_label(const char *iso, char *out, size_t cap)
{
    char today[32], yday[32];
    out[0] = '\0';
    if (!iso || strlen(iso) < 10) return;
    aic_ms_to_iso8601(aic_now_ms(), today);
    aic_ms_to_iso8601(aic_now_ms() - 86400000LL, yday);
    if (strncmp(iso, today, 10) == 0) { aic_strlcpy_utf8(out, cap, "今天"); return; }
    if (strncmp(iso, yday, 10) == 0) { aic_strlcpy_utf8(out, cap, "昨天"); return; }
    {
        char d[11];
        memcpy(d, iso, 10);
        d[10] = '\0';
        aic_strlcpy_utf8(out, cap, d);
    }
}

static int same_date(const char *x, const char *y)
{
    if (!x || !y || strlen(x) < 10 || strlen(y) < 10) return 0;
    return strncmp(x, y, 10) == 0;
}

static void paint_messages(App *a, HDC dc, RECT area)
{
    int n = (int)a->msg_n;
    int cap, start_idx, i;
    int *hs = NULL, *seps = NULL, *gaps = NULL;
    int total = 0, y;
    int bubble_max = area.right - area.left - 160;

    if (bubble_max > 560) bubble_max = 560;
    if (bubble_max < 160) bubble_max = 160;

    if (n <= 0) {
        if (a->sending && a->stream_len > 0) {
            char live0[65536];
            RECT br0;
            int hh0, tw0 = 0;
            md_strip(a->stream_text, live0, sizeof(live0));
            hh0 = msg_height(dc, a, live0, bubble_max - 24, &tw0);
            tw0 += 24;
            if (tw0 < 120) tw0 = 120;
            br0.left = area.left + 16;
            br0.right = br0.left + tw0;
            br0.top = area.top + 8;
            br0.bottom = br0.top + hh0;
            fill_round(dc, br0, 12, tok(a, AIC_TOK_CARD_BG));
            stroke_round(dc, br0, 12, tok(a, AIC_TOK_PRIMARY), 1);
            msg_draw(a, dc, br0.top + 10, br0.left + 12, br0.right - 12, live0, bubble_max - 24,
                     -1, tok(a, AIC_TOK_TEXT));
        } else {
            RECT r = {area.left, area.top + 40, area.right, area.top + 70};
            draw_text(dc, a->font_ui,
                      aic_router_mode(a->router) == AIC_MODE_QA
                          ? "这个会话还没有消息。输入内容开始一次问答。"
                          : "还没有消息。说点什么开始今天的聊天吧。",
                      r, tok(a, AIC_TOK_TEXT_SECONDARY), DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
        return;
    }

    cap = n > MSG_CAP ? MSG_CAP : n;
    start_idx = n - cap;
    hs = (int *)aic_xmalloc(sizeof(int) * (size_t)cap);
    seps = (int *)aic_xmalloc(sizeof(int) * (size_t)cap);
    gaps = (int *)aic_xmalloc(sizeof(int) * (size_t)cap);

    for (i = 0; i < cap; i++) {
        aic_message *m = &a->msgs[start_idx + i];
        const char *txt = m->recalled ? AIC_RECALL_NOTICE : (m->content ? m->content : "");
        int w = 0;
        hs[i] = msg_height(dc, a, txt, bubble_max - 24, &w);
        if (hs[i] < 40) hs[i] = 40;
        seps[i] = 0;
        if (i == 0 || !same_date(a->msgs[start_idx + i - 1].created_at, m->created_at)) seps[i] = 28;
        total += hs[i] + seps[i];
    }
    for (i = 0; i < cap; i++) {
        int same_role = 0;
        if (i + 1 < cap) {
            aic_message *x = &a->msgs[start_idx + i];
            aic_message *z = &a->msgs[start_idx + i + 1];
            same_role = (strcmp(x->role, z->role) == 0) && same_date(x->created_at, z->created_at);
        }
        gaps[i] = same_role ? 4 : 10;
        total += gaps[i];
    }
    if (a->sending && a->stream_len > 0) {
        char live_h0[65536];
        int hh;
        md_strip(a->stream_text, live_h0, sizeof(live_h0));
        hh = msg_height(dc, a, live_h0, bubble_max - 24, NULL);
        if (hh < 40) hh = 40;
        total += hh + 14;
    }

    {
        int view_h = area.bottom - area.top;
        if (total <= view_h) a->scroll[2] = 0;
        else if (a->scroll[2] > total - view_h) a->scroll[2] = total - view_h;
        if (a->scroll[2] < 0) a->scroll[2] = 0;
    }

    y = area.bottom - 6 - total + a->scroll[2];

    for (i = 0; i < cap; i++) {
        aic_message *m = &a->msgs[start_idx + i];
        const char *txt = m->recalled ? AIC_RECALL_NOTICE : (m->content ? m->content : "");
        int h = hs[i];
        int idx = start_idx + i;
        int is_user = strcmp(m->role, "user") == 0;
        int is_sys = strcmp(m->role, "system") == 0;

        if (seps[i] && y + seps[i] >= area.top - 20) {
            char lbl[32];
            RECT sr = {area.left, y + 4, area.right, y + seps[i] - 2};
            date_label(m->created_at, lbl, sizeof(lbl));
            if (lbl[0])
                draw_text(dc, a->font_small, lbl, sr, tok(a, AIC_TOK_TEXT_SECONDARY),
                          DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
        y += seps[i];

        if (y + h >= area.top && y <= area.bottom) {
            if (is_sys) {
                RECT tr = {area.left + 24, y, area.right - 24, y + h};
                draw_text(dc, a->font_small, txt, tr, tok(a, AIC_TOK_TEXT_SECONDARY),
                          DT_CENTER | DT_TOP | DT_WORDBREAK);
            } else {
                int tw = 0;
                RECT br;
                COLORREF fg = is_user ? RGB(255, 255, 255)
                                      : (m->recalled ? tok(a, AIC_TOK_TEXT_SECONDARY)
                                                     : tok(a, AIC_TOK_TEXT));
                msg_height(dc, a, txt, bubble_max - 24, &tw);
                tw += 24;
                if (tw < 72) tw = 72;
                if (is_user) {
                    br.right = area.right - 16;
                    br.left = br.right - tw;
                } else {
                    br.left = area.left + 16;
                    br.right = br.left + tw;
                }
                br.top = y;
                br.bottom = y + h;
                fill_round(dc, br, 12, is_user ? tok(a, AIC_TOK_PRIMARY) : tok(a, AIC_TOK_CARD_BG));
                if (!is_user) stroke_round(dc, br, 12, tok(a, AIC_TOK_DIVIDER), 1);
                msg_draw(a, dc, br.top + 10, br.left + 12, br.right - 12, txt, bubble_max - 24,
                         idx, fg);
                if (m->edited_at[0] && !m->recalled) {
                    RECT er = {br.left + 12, br.bottom - 18, br.right - 12, br.bottom - 2};
                    draw_text(dc, a->font_small, "已修改", er, tok(a, AIC_TOK_TEXT_SECONDARY),
                              DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
                }
                reg(br, ACT_MSG_BASE + idx);
            }
        }
        y += h + gaps[i];
    }

    if (a->sending && a->stream_len > 0) {
        char live[65536];
        int hh, tw = 0;
        RECT br;
        md_strip(a->stream_text, live, sizeof(live));
        hh = msg_height(dc, a, live, bubble_max - 24, &tw);
        if (hh < 40) hh = 40;
        tw += 24;
        if (tw < 120) tw = 120;
        br.left = area.left + 16;
        br.right = br.left + tw;
        br.top = y;
        br.bottom = y + hh;
        if (br.bottom >= area.top && br.top <= area.bottom) {
            fill_round(dc, br, 12, tok(a, AIC_TOK_CARD_BG));
            stroke_round(dc, br, 12, tok(a, AIC_TOK_PRIMARY), 1);
            msg_draw(a, dc, br.top + 10, br.left + 12, br.right - 12, live, bubble_max - 24, -1,
                     tok(a, AIC_TOK_TEXT));
        }
    }

    aic_xfree(hs);
    aic_xfree(seps);
    aic_xfree(gaps);
}

static void paint_input(App *a, HDC dc, RECT area)
{
    RECT box = {area.left + 16, area.top + 8, area.right - 112, area.bottom - 12};
    RECT btn = {area.right - 100, area.bottom - 52, area.right - 16, area.bottom - 20};
    RECT tr;
    RECT hint = {area.left + 20, area.bottom - 20, area.right - 120, area.bottom - 2};

    fill_round(dc, box, 10, tok(a, AIC_TOK_CARD_BG));
    stroke_round(dc, box, 10, tok(a, AIC_TOK_DIVIDER), 1);

    tr.left = box.left + 12;
    tr.right = box.right - 12;
    tr.top = box.top + 8;
    tr.bottom = box.bottom - 8;

    if (a->draft_len == 0) {
        draw_text(dc, a->font_ui, "写下你想说的话，Enter 发送", tr,
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        draw_text(dc, a->font_ui, a->draft, tr, tok(a, AIC_TOK_TEXT),
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        if (a->input_focus && a->caret_on) {
            int w = 0;
            int n = (int)measure_text(dc, a->font_ui, a->draft, tr.right - tr.left, &w);
            RECT cr = {tr.left + (w > 0 ? w : 0), tr.top, tr.left + (w > 0 ? w : 0) + 1,
                       tr.top + (n > 0 ? n : 16)};
            HBRUSH b = CreateSolidBrush(tok(a, AIC_TOK_TEXT));
            FillRect(dc, &cr, b);
            DeleteObject(b);
        }
    }
    reg(box, ACT_SEND);

    if (a->sending) draw_pill(a, dc, btn, "停止", ACT_STOP, 1);
    else draw_pill(a, dc, btn, "发送", ACT_SEND, 1);
    {
        char line[256];
        snprintf(line, sizeof(line), "%s · 已输入 %d 字 · 上下文 %d 条",
                 aic_router_mode(a->router) == AIC_MODE_CHAT ? "聊天模式 · 单会话" : "问答模式 · 多会话",
                 (int)aic_utf8_length(a->draft),
                 aic_setting_get_int(a->db, AIC_SET_CONTEXT_N, AIC_CONTEXT_MESSAGES_DEFAULT));
        draw_text(dc, a->font_small, line, hint, tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    }
}

static void paint_chat(App *a, HDC dc, RECT content)
{
    RECT header = {content.left, content.top, content.right, content.top + 64};
    RECT sep = {header.left, header.bottom - 1, header.right, header.bottom};
    RECT title = {header.left + 20, header.top + 8, header.right - 260, header.top + 38};
    RECT sub = {header.left + 20, header.top + 34, header.right - 260, header.top + 56};
    RECT info = {content.right - 132, header.top + 14, content.right - 20, header.top + 46};
    RECT area = {content.left, header.bottom + 4, content.right, content.bottom - INPUT_H - 8};
    RECT inarea = {content.left, content.bottom - INPUT_H, content.right, content.bottom};
    HBRUSH b = CreateSolidBrush(tok(a, AIC_TOK_DIVIDER));
    const char *t = a->conv_title[0] ? a->conv_title : "聊天";
    char subline[256];

    if (aic_router_mode(a->router) == AIC_MODE_QA) {
        RECT back = {content.left + 8, header.top + 14, content.left + 76, header.top + 46};
        draw_pill(a, dc, back, "列表", ACT_QA_BACK, 0);
        title.left = content.left + 88;
        sub.left = content.left + 88;
    }

    {
        /* AI 头像（技术设计 6.4）：选了头像就画成圆形，没选则不占位。 */
        char rel[600] = {0};
        int base = (aic_router_mode(a->router) == AIC_MODE_QA) ? content.left + 88 : header.left + 20;
        aic_setting_get(a->db, "persona.avatar", rel, sizeof(rel));
        if (rel[0]) {
            char full[900];
            aic_path_join(full, sizeof(full), a->root, rel);
            if (aic_path_exists(full)) {
                RECT av = {base, header.top + 18, base + 28, header.top + 46};
                HRGN clip = CreateEllipticRgn(av.left, av.top, av.right, av.bottom);
                HBRUSH bg = CreateSolidBrush(tok(a, AIC_TOK_PRIMARY));
                SelectClipRgn(dc, clip);
                FillRect(dc, &av, bg);
                aic_avatar_draw(dc, a->root, rel, av.left, av.top, 28);
                SelectClipRgn(dc, NULL);
                DeleteObject(bg);
                DeleteObject(clip);
                title.left = av.right + 10;
                sub.left = av.right + 10;
            }
        }
    }
    draw_text(dc, a->font_title, t, title, tok(a, AIC_TOK_TEXT),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    snprintf(subline, sizeof(subline), "人设：%s · 模型：%s",
             a->persona_name[0] ? a->persona_name : "默认陪伴人设",
             aic_llm_model_or_default(
                 aic_setting_get_str(a->db, AIC_SET_MODEL_DEFAULT, AIC_MODEL_FLASH, NULL, 0)
                     ? aic_setting_get_str(a->db, AIC_SET_MODEL_DEFAULT, AIC_MODEL_FLASH, NULL, 0)
                     : AIC_MODEL_FLASH)->name);
    draw_text(dc, a->font_small, subline, sub, tok(a, AIC_TOK_TEXT_SECONDARY),
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_pill(a, dc, info, "AI 信息", ACT_AI_INFO, 0);
    {
        RECT regen = {content.right - 248, header.top + 14, content.right - 144, header.top + 46};
        if (a->msg_n > 0) draw_pill(a, dc, regen, "重新生成", ACT_REGENERATE, 0);
    }
    {
        /* 撤回 / 修改后的短时撤销入口（技术设计 9.2）。 */
        if (a->undo_msg[0] && aic_now_ms() <= a->undo_until && a->msg_n > 0) {
            RECT undo = {content.right - 348, header.top + 14, content.right - 256, header.top + 46};
            draw_pill(a, dc, undo, "撤销", ACT_UNDO, 0);
        }
    }
    if (aic_router_mode(a->router) == AIC_MODE_QA && a->conv_id[0]) {
        RECT ren = {content.right - 448, header.top + 14, content.right - 356, header.top + 46};
        draw_pill(a, dc, ren, "改名", ACT_QA_RENAME, 0);
    }

    FillRect(dc, &sep, b);
    DeleteObject(b);

    {
        /* §9.4：错误只走输入区上方的非侵入状态条，不进聊天框、也不挤占消息本身。 */
        int unread = aic_error_unread(a->db);
        if (unread > 0) {
            RECT bar;
            char txt[160];
            /* 条子钉在输入区上方，消息区底部让出同样的高度，互不遮挡。 */
            bar.left = content.left + 12;
            bar.right = content.right - 12;
            bar.bottom = inarea.top - 8;
            bar.top = bar.bottom - 28;
            area.bottom = bar.top - 6;
            fill_round(dc, bar, 8, tok(a, AIC_TOK_CARD_BG));
            stroke_round(dc, bar, 8, tok(a, AIC_TOK_DIVIDER), 1);
            snprintf(txt, sizeof(txt), "有 %d 条运行错误未查看，点这里进错误中心", unread);
            draw_text(dc, a->font_small, txt,
                      (RECT){bar.left + 12, bar.top, bar.right - 12, bar.bottom},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            reg(bar, ACT_ROW_ERROR);
        }
    }
    if (a->msg_has_more) {
        /* 技术设计 13.3：还有更早的消息时给一个明确的入口。 */
        RECT more = {content.right - 132, area.top + 4, content.right - 20, area.top + 34};
        draw_pill(a, dc, more, "加载更早", ACT_LOAD_OLDER, 0);
    }
    paint_messages(a, dc, area);
    paint_input(a, dc, inarea);
}

/* ---------- 「我」 ---------- */
static void paint_me(App *a, HDC dc, RECT content)
{
    aic_profile p;
    RECT y = content;
    RECT avatar;
    HBRUSH b;
    HPEN pen;
    char line[640];

    memset(&p, 0, sizeof(p));
    aic_profile_get(a->db, &p);

    y.left += PAD + 8;
    y.right -= PAD + 8;
    y.top += 8;

    draw_text(dc, a->font_title, "我", (RECT){y.left, y.top, y.right, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 56;

    avatar.left = y.left;
    avatar.top = y.top;
    avatar.right = y.left + 72;
    avatar.bottom = y.top + 72;
    b = CreateSolidBrush(tok(a, AIC_TOK_PRIMARY));
    pen = CreatePen(PS_SOLID, 1, tok(a, AIC_TOK_PRIMARY));
    {
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, pen);
        Ellipse(dc, avatar.left, avatar.top, avatar.right, avatar.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
    }
    DeleteObject(b);
    DeleteObject(pen);

    {
        int drew = 0;
        if (p.avatar_path[0]) {
            char ap[900];
            aic_path_join(ap, sizeof(ap), a->root, p.avatar_path);
            if (aic_path_exists(ap))
                drew = (aic_avatar_draw(dc, a->root, p.avatar_path, avatar.left, avatar.top, 72) == 1);
        }
        if (drew) {
            /* 头像图片已绘制，不再叠加首字。 */
        } else {
        char one[8];
        if (p.name[0]) {
            aic_strlcpy_utf8(one, sizeof(one), p.name);
            aic_utf8_truncate_chars(one, 1);
        } else {
            aic_strlcpy_utf8(one, sizeof(one), "我");
        }
        draw_text(dc, a->font_title, one, avatar, RGB(255, 255, 255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    {
        RECT nr = {avatar.right + 16, avatar.top + 8, y.right, avatar.top + 40};
        RECT sr = {avatar.right + 16, avatar.top + 38, y.right, avatar.top + 64};
        draw_text(dc, a->font_title, p.name[0] ? p.name : "未设置昵称", nr,
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        snprintf(line, sizeof(line), "%s · %s · %s",
                 p.gender[0] ? p.gender : "未设置", p.birthday[0] ? p.birthday : "生日未填",
                 p.occupation[0] ? p.occupation : "职业未填");
        draw_text(dc, a->font_small, line, sr, tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    y.top += 88;

    {
        RECT btn = {y.left, y.top, y.left + 120, y.top + 36};
        RECT btn2 = {y.left + 132, y.top, y.left + 252, y.top + 36};
        draw_pill(a, dc, btn, "编辑资料", ACT_PROFILE_EDIT, 1);
        draw_pill(a, dc, btn2, "重看指南", ACT_ROW_GUIDE, 0);
        y.top += 52;
    }

    draw_row(a, dc, &y, "性别", p.gender[0] ? p.gender : "未设置", 0);
    draw_row(a, dc, &y, "生日", p.birthday[0] ? p.birthday : "未填写", 0);
    draw_row(a, dc, &y, "职业", p.occupation[0] ? p.occupation : "未填写", 0);
    draw_row(a, dc, &y, "头像路径", p.avatar_path[0] ? p.avatar_path : "未设置", 0);
    draw_row(a, dc, &y, "补充说明",
             p.extra_note[0] ? p.extra_note : "未填写", 0);

    y.top += 16;
    {
        RECT r = {y.left, y.top, y.right, y.top + 80};
        fill_round(dc, r, 10, tok(a, AIC_TOK_CARD_BG));
        draw_text(dc, a->font_small,
                  "用户资料会作为变量写进提示词：{user_name} {user_gender} {user_birthday} "
                  "{user_occupation} {user_extra_note}，未填写的替换为空串。",
                  (RECT){r.left + 14, r.top + 10, r.right - 14, r.bottom - 10},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

/* ---------- 设置 ---------- */
static void paint_settings(App *a, HDC dc, RECT content)
{
    aic_guide_state gs;
    aic_persona per;
    aic_plugin *plgs = NULL;
    size_t plg_n = 0;
    RECT y = content;
    char v[160];
    char theme[16] = {0};
    char model[64] = {0};
    int has_key = 0;

    if (a->page_kb) { paint_kb(a, dc, content); return; }
    if (a->page_error) { paint_error_center(a, dc, content); return; }
    if (a->page_cap) { paint_capability(a, dc, content); return; }
    if (a->page_advanced) { paint_advanced(a, dc, content); return; }
    if (a->page_pair) { paint_pair(a, dc, content); return; }

    aic_guide_state_get(a->db, &gs);
    memset(&per, 0, sizeof(per));
    aic_persona_ensure_default(a->db, &per);
    aic_plugin_list(a->db, -1, &plgs, &plg_n);
    aic_setting_get(a->db, AIC_SET_THEME, theme, sizeof(theme));
    aic_setting_get(a->db, AIC_SET_MODEL_DEFAULT, model, sizeof(model));
    has_key = aic_engine_has_key(a->eng);

    y.left += PAD + 8;
    y.right -= PAD + 8;
    {
        /* 19 行设置项 + 标题 + 底部说明：滚动范围收敛到真实内容高度，避免滚过底。 */
        int needed = 52 + 19 * ROW_H + 12 + 80;
        int avail = (int)(content.bottom - content.top);
        int maxs = needed - avail;
        if (maxs < 0) maxs = 0;
        if (a->scroll[1] > maxs) a->scroll[1] = maxs;
        if (a->scroll[1] < 0) a->scroll[1] = 0;
    }
    y.top += 8 - a->scroll[1];

    draw_text(dc, a->font_title, "设置", (RECT){y.left, y.top, y.right, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 52;

    snprintf(v, sizeof(v), "%s", per.name);
    draw_row(a, dc, &y, "提示词与人设", v, ACT_AI_INFO);

    if (theme[0] == '1' || aic_str_ieq(theme, "night")) aic_strlcpy_utf8(v, sizeof(v), "夜间");
    else if (theme[0] == '2' || aic_str_ieq(theme, "system")) aic_strlcpy_utf8(v, sizeof(v), "跟随系统");
    else aic_strlcpy_utf8(v, sizeof(v), "日光");
    draw_row(a, dc, &y, "页面配色", v, ACT_ROW_THEME);

    draw_row(a, dc, &y, "模型",
             aic_llm_model_or_default(model[0] ? model : AIC_MODEL_FLASH)->name, ACT_ROW_MODEL);

    draw_row(a, dc, &y, "API Key", has_key ? "已配置（系统安全存储）" : "未配置", ACT_ROW_KEY);
    draw_row(a, dc, &y, "余额与用量", "查看本地账本", ACT_ROW_BALANCE);
    draw_row(a, dc, &y, "记录导出", "JSON / Markdown / 纯文本", ACT_ROW_EXPORT);
    draw_row(a, dc, &y, "记录导入", "校验后按冲突策略写入", ACT_ROW_IMPORT);

    /* 第 16 章：实时同步与配对管理（§7.2 设置项的两行）。 */
    draw_row(a, dc, &y, "实时同步",
             aic_sync_realtime_enabled(a->db) ? "已开启" : "已暂停", ACT_ROW_SYNC);
    {
        aic_pair_record rec;
        if (aic_pairing_current(a->db, &rec) == AIC_OK)
            snprintf(v, sizeof(v), "已配对：%s", rec.peer_name[0] ? rec.peer_name : rec.peer_id);
        else
            aic_strlcpy_utf8(v, sizeof(v), "未配对，生成配对码");
        draw_row(a, dc, &y, "配对管理", v, ACT_ROW_PAIR);
    }

    {
        int enabled_n = 0;
        for (size_t i = 0; i < plg_n; i++) if (plgs[i].enabled) enabled_n++;
        snprintf(v, sizeof(v), "%d 个插件 · %d 个启用", (int)plg_n, enabled_n);
    }
    draw_row(a, dc, &y, "插件", v, ACT_ROW_PLUGIN);

    draw_row(a, dc, &y, "更新设置", "检查 / 校验 / 备份前置", ACT_ROW_UPDATE);

    snprintf(v, sizeof(v), "已完成 %d / 9 步", gs.completed_count);
    draw_row(a, dc, &y, "操作指南", v, ACT_ROW_GUIDE);

    draw_row(a, dc, &y, "数据管理", "数据库 / 备份 / 清理", ACT_ROW_DATA);
    draw_row(a, dc, &y, "进阶指南", "插件制作 / 变量 / 迁移 / 排错", ACT_ROW_ADVANCED);

    /* §7.2 补上的三行：内置知识库（FR-31/32）、设备权限（FR-35）、错误中心（FR-34）。 */
    app_kb_refresh(a);
    if (a->kb)
        snprintf(v, sizeof(v), "%d 篇 · %d 组 · 书签 %d", a->kb->article_n,
                 aic_kb_group_count(a->kb), aic_kb_bookmark_count(a->db));
    else
        aic_strlcpy_utf8(v, sizeof(v), "知识包未载入");
    draw_row(a, dc, &y, "内置知识库", v, ACT_ROW_KB);
    {
        int granted = 0;
        int ci;
        int cap_n = (int)aic_cap_count();
        for (ci = 0; ci < cap_n; ci++) {
            const char *k = aic_cap_kind_at(ci);
            if (k && aic_cap_is_granted(a->db, k)) granted++;
        }
        snprintf(v, sizeof(v), "已授权 %d / %d 项", granted, cap_n);
    }
    draw_row(a, dc, &y, "设备权限", v, ACT_ROW_CAP);
    {
        int unread = aic_error_unread(a->db);
        if (unread > 0) snprintf(v, sizeof(v), "%d 条未查看", unread);
        else aic_strlcpy_utf8(v, sizeof(v), "无未读");
    }
    draw_row(a, dc, &y, "错误中心", v, ACT_ROW_ERROR);
    draw_row(a, dc, &y, "减少动态效果",
             aic_setting_get_int(a->db, AIC_SET_MOTION, 1) == 0 ? "开启" : "关闭", ACT_ROW_MOTION);
    draw_row(a, dc, &y, "关于", "Sai " APP_VERSION, ACT_ROW_ABOUT);

    y.top += PAD;
    draw_text(dc, a->font_small,
              "凭证只写入系统安全存储（Windows 凭据管理器），界面与日志不出现明文；"
              "导出包不包含凭证。",
              (RECT){y.left + 4, y.top, y.right - 4, y.top + 60},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);

    aic_persona_free(&per);
    if (plgs) aic_plugin_free_array(plgs, plg_n);
}

/* ---------- 问答模式：会话列表 ---------- */
static void paint_qa(App *a, HDC dc, RECT content)
{
    const char *tid = aic_router_thread(a->router);
    RECT y = content;
    RECT search;
    RECT other;

    if (tid && tid[0]) {
        paint_chat(a, dc, content);
        return;
    }

    y.left += PAD + 8;
    y.right -= PAD + 8;
    y.top += 8;

    draw_text(dc, a->font_title, "问答会话", (RECT){y.left, y.top, y.right - 220, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    other.left = y.right - 200;
    other.top = y.top + 8;
    other.right = y.right - 120;
    other.bottom = y.top + 40;
    draw_pill(a, dc, other, "设置", ACT_NAV_SETTINGS, 0);
    other.left = y.right - 108;
    other.right = y.right;
    draw_pill(a, dc, other, "新建会话", ACT_QA_NEW, 1);
    y.top += 56;

    search.left = y.left;
    search.top = y.top;
    search.right = y.right;
    search.bottom = y.top + 40;
    fill_round(dc, search, 10, tok(a, AIC_TOK_CARD_BG));
    stroke_round(dc, search, 10, tok(a, AIC_TOK_DIVIDER), 1);
    draw_text(dc, a->font_ui, a->search[0] ? a->search : "搜索会话标题…",
              (RECT){search.left + 12, search.top, search.right - 12, search.bottom},
              a->search[0] ? tok(a, AIC_TOK_TEXT) : tok(a, AIC_TOK_TEXT_SECONDARY),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    reg(search, ACT_QA_SEARCH);
    y.top += 52;

    if (a->thread_n == 0) {
        draw_text(dc, a->font_ui, a->search[0] ? "没有匹配的会话。" : "还没有问答会话，点右上角新建一个。",
                  (RECT){y.left + 4, y.top + 12, y.right - 4, y.top + 44},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_SINGLELINE);
        return;
    }

    for (size_t i = 0; i < a->thread_n && y.top < content.bottom - 40; i++) {
        RECT rr = {y.left, y.top, y.right, y.top + 64};
        RECT t1 = {rr.left + 16, rr.top + 8, rr.right - 120, rr.top + 36};
        RECT t2 = {rr.left + 16, rr.top + 34, rr.right - 120, rr.top + 58};
        char sub[128];
        if (a->threads[i].pinned)
            fill_round(dc, (RECT){rr.left, rr.top, rr.left + 4, rr.top + 64}, 0,
                       tok(a, AIC_TOK_ACCENT));
        draw_text(dc, a->font_ui,
                  a->threads[i].title[0] ? a->threads[i].title : "未命名会话", t1,
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        snprintf(sub, sizeof(sub), "%s · %lld 条消息", a->threads[i].updated_at,
                 a->threads[i].last_seq);
        draw_text(dc, a->font_small, sub, t2, tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        {
            RECT dl = {rr.left, rr.bottom - 1, rr.right, rr.bottom};
            HBRUSH b = CreateSolidBrush(tok(a, AIC_TOK_DIVIDER));
            FillRect(dc, &dl, b);
            DeleteObject(b);
        }
        reg(rr, ACT_QA_THREAD_BASE + (int)i);
        y.top += 66;
    }
}

/* ---------- 浮层 ---------- */
static void paint_overlay(App *a, HDC dc, RECT client)
{
    int w = 560;
    int x;
    int y = client.top + 80;
    int bottom;
    int field_h = 0;
    RECT panel;
    RECT tr;

    if (a->overlay == AIC_OV_NONE) return;

    for (int i = 0; i < a->field_n; i++) field_h += a->fields[i].multiline ? 112 : 68;

    x = (client.right - w) / 2;
    /* 面板高度按正文实测行数算，正文短也不会留空、正文长也不会压住底部按钮。 */
    {
        int th = 0;
        if (a->overlay_text[0] && a->overlay != AIC_OV_GUIDE) {
            th = measure_text(dc, a->font_ui, a->overlay_text, w - 40, NULL);
            if (th > 240) th = 240;
        }
        bottom = y + 62 + th + field_h
                 + (a->overlay == AIC_OV_GUIDE ? AIC_GUIDE_STEP_COUNT * 46 + 40 : 0) + 88;
    }
    if (a->confirm_action == FORM_EXPORT) bottom += 48;
    if (a->confirm_action == FORM_PERSONA) bottom += 48;
    if (a->confirm_action == FORM_VERSIONS) bottom += a->ver_n * 46 + 24;
    if (bottom > client.bottom - 20) bottom = client.bottom - 20;
    panel.left = x;
    panel.top = y;
    panel.right = x + w;
    panel.bottom = bottom;

    fill_round(dc, panel, 14, tok(a, AIC_TOK_CARD_BG));
    stroke_round(dc, panel, 14, tok(a, AIC_TOK_DIVIDER), 1);

    draw_text(dc, a->font_title, a->overlay_title,
              (RECT){panel.left + 20, panel.top + 12, panel.right - 20, panel.top + 46},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    tr.left = panel.left + 20;
    tr.right = panel.right - 20;
    tr.top = panel.top + 50;
    tr.bottom = tr.top + 44;

    if (a->overlay == AIC_OV_GUIDE) {
        const aic_guide_step *steps = aic_guide_steps();
        aic_guide_state gs;
        aic_guide_state_get(a->db, &gs);
        for (int i = 0; i < AIC_GUIDE_STEP_COUNT; i++) {
            RECT r = {tr.left, tr.top, tr.right, tr.top + 42};
            char label[256];
            snprintf(label, sizeof(label), "%s %s",
                     gs.completed[i] ? "[已完成]" : (gs.skipped[i] ? "[已跳过]" : "[待完成]"),
                     steps[i].title);
            if (gs.completed[i] || gs.skipped[i])
                fill_round(dc, r, 8, tok(a, AIC_TOK_PAGE_BG));
            draw_text(dc, a->font_ui, label,
                      (RECT){r.left + 12, r.top, r.right - 12, r.top + 22},
                      tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, a->font_small, steps[i].done_when,
                      (RECT){r.left + 12, r.top + 20, r.right - 12, r.bottom},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            reg(r, ACT_GUIDE_ITEM_BASE + i);
            tr.top += 46;
        }
        {
            RECT b1 = {panel.left + 20, panel.bottom - 52, panel.left + 140, panel.bottom - 16};
            RECT b2 = {panel.right - 120, panel.bottom - 52, panel.right - 20, panel.bottom - 16};
            draw_pill(a, dc, b1, "重置进度", ACT_GUIDE_RESET, 0);
            draw_pill(a, dc, b2, "关闭", ACT_OVERLAY_CLOSE, 1);
        }
        return;
    }

    if (a->overlay_text[0]) {
        int h = measure_text(dc, a->font_ui, a->overlay_text, tr.right - tr.left, NULL);
        if (h > 240) h = 240;
        draw_text(dc, a->font_ui, a->overlay_text,
                  (RECT){tr.left, tr.top, tr.right, tr.top + h},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
        tr.top += h + 12;
    }

    if (a->confirm_action == FORM_PLUGIN) {
        for (size_t i = 0; i < a->plug_n && tr.top < panel.bottom - 96; i++) {
            aic_plugin *pl = &a->plugs[i];
            RECT row = {tr.left, tr.top, tr.right, tr.top + 44};
            RECT tg = {row.right - 168, row.top + 6, row.right - 92, row.top + 38};
            RECT up = {row.right - 84, row.top + 6, row.right, row.top + 38};
            char label[256];
            fill_round(dc, row, 8, tok(a, AIC_TOK_PAGE_BG));
            snprintf(label, sizeof(label), "%s  v%s%s", pl->name, pl->version,
                     pl->enabled ? "（已启用）" : "（已禁用）");
            draw_text(dc, a->font_ui, label,
                      (RECT){row.left + 12, row.top + 4, tg.left - 8, row.bottom - 4},
                      tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_pill(a, dc, tg, pl->enabled ? "禁用" : "启用",
                      ACT_PLUGIN_TOGGLE_BASE + (int)i, 0);
            draw_pill(a, dc, up, "更新", ACT_PLUGIN_UPDATE_BASE + (int)i, 0);
            tr.top += 50;
        }
        if (a->plug_n == 0) {
            draw_text(dc, a->font_small, "还没有安装插件。先用「导入插件」选择一个 plugin.json。",
                      (RECT){tr.left, tr.top, tr.right, tr.top + 24},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            tr.top += 28;
        }
    }

    if (a->confirm_action == FORM_VERSIONS) {
        for (int i = 0; i < a->ver_n; i++) {
            RECT row = {tr.left, tr.top, tr.right, tr.top + 40};
            RECT bt = {row.right - 108, row.top + 4, row.right, row.top + 36};
            char lab[96];
            fill_round(dc, row, 8, tok(a, AIC_TOK_PAGE_BG));
            snprintf(lab, sizeof(lab), "v%d    %s", a->ver_num[i], a->ver_time[i]);
            draw_text(dc, a->font_ui, lab,
                      (RECT){row.left + 12, row.top, bt.left - 8, row.bottom},
                      tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_pill(a, dc, bt, "回退", ACT_PERSONA_ROLLBACK_BASE + i, 0);
            tr.top += 46;
        }
        if (a->ver_n == 0) {
            draw_text(dc, a->font_small, "还没有历史版本。",
                      (RECT){tr.left, tr.top, tr.right, tr.top + 24},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            tr.top += 28;
        }
    }

    for (int i = 0; i < a->field_n; i++) {
        Field *fl = &a->fields[i];
        int bh = fl->multiline ? 88 : 36;
        RECT lab = {tr.left, tr.top, tr.right, tr.top + 22};
        RECT box = {tr.left, tr.top + 22, tr.right, tr.top + 22 + bh};
        draw_text(dc, a->font_small, fl->label, lab, tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        fill_round(dc, box, 8, tok(a, AIC_TOK_PAGE_BG));
        stroke_round(dc, box, 8,
                     a->field_focus == i ? tok(a, AIC_TOK_PRIMARY) : tok(a, AIC_TOK_DIVIDER), 1);
        draw_text(dc, a->font_ui, fl->buf[0] ? fl->buf : "（空）",
                  (RECT){box.left + 10, box.top + 4, box.right - 10, box.bottom - 4},
                  fl->buf[0] ? tok(a, AIC_TOK_TEXT) : tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        reg(box, ACT_FIELD_BASE + i);
        tr.top += fl->multiline ? 112 : 68;
    }

    {
        int fy = panel.bottom - 52;
        RECT b1 = {panel.left + 20, fy, panel.left + 140, fy + 36};
        RECT b2 = {panel.left + 152, fy, panel.left + 272, fy + 36};
        RECT b3 = {panel.right - 120, fy, panel.right - 20, fy + 36};

        if (a->overlay == AIC_OV_GUIDE) return;

        switch (a->confirm_action) {
        case FORM_PERSONA: {
            /* AI 信息界面（技术设计 6.4）：AI 头像、模型偏好、提示词版本回退。 */
            RECT r1 = {panel.left + 20, fy - 44, panel.left + 124, fy - 8};
            RECT r2 = {panel.left + 132, fy - 44, panel.left + 268, fy - 8};
            RECT r3 = {panel.left + 276, fy - 44, panel.left + 392, fy - 8};
            RECT r4 = {panel.left + 400, fy - 44, panel.left + 500, fy - 8};
            char cur[64] = {0};
            aic_setting_get(a->db, AIC_SET_MODEL_DEFAULT, cur, sizeof(cur));
            draw_pill(a, dc, r1, "AI 头像", ACT_AI_AVATAR, 0);
            draw_pill(a, dc, r2, "Flash", ACT_AI_MODEL_FLASH, aic_str_ieq(cur, AIC_MODEL_FLASH));
            draw_pill(a, dc, r3, "Pro", ACT_AI_MODEL_PRO, aic_str_ieq(cur, AIC_MODEL_PRO));
            draw_pill(a, dc, r4, "版本回退", ACT_AI_VERSIONS, 0);
            draw_pill(a, dc, b1, "保存", ACT_FIELD_SAVE, 1);
            draw_pill(a, dc, b3, "取消", ACT_FIELD_CANCEL, 0);
            break;
        }
        case FORM_KEY:
        case FORM_PROFILE:
        case FORM_EDITMSG:
        case FORM_RENAME:
            draw_pill(a, dc, b1, "保存", ACT_FIELD_SAVE, 1);
            draw_pill(a, dc, b3, "取消", ACT_FIELD_CANCEL, 0);
            break;
        case FORM_EXPORT: {
            /* 导出范围（技术设计 8.1）：范围 + 是否含头像。 */
            RECT r1 = {panel.left + 20, fy - 44, panel.left + 140, fy - 8};
            RECT r2 = {panel.left + 148, fy - 44, panel.left + 248, fy - 8};
            RECT r3 = {panel.left + 256, fy - 44, panel.left + 348, fy - 8};
            RECT r4 = {panel.left + 356, fy - 44, panel.left + 452, fy - 8};
            draw_pill(a, dc, r1, "当前会话", ACT_SCOPE_CUR, a->export_scope == 0);
            draw_pill(a, dc, r2, "全部会话", ACT_SCOPE_ALL, a->export_scope == 1);
            draw_pill(a, dc, r3, "近 7 天", ACT_SCOPE_RANGE, a->export_scope == 2);
            draw_pill(a, dc, r4, "含头像", ACT_SCOPE_AVATAR, a->export_avatars ? 1 : 0);
            draw_pill(a, dc, b1, "JSON", ACT_EXPORT_JSON, 1);
            draw_pill(a, dc, b2, "Markdown", ACT_EXPORT_MD, 0);
            draw_pill(a, dc, b3, "纯文本", ACT_EXPORT_TXT, 0);
            break;
        }
        case FORM_VERSIONS:
            draw_pill(a, dc, b1, "编辑人设", ACT_AI_INFO, 1);
            draw_pill(a, dc, b3, "关闭", ACT_OVERLAY_CLOSE, 0);
            break;
        case FORM_CONFIRM:
            draw_pill(a, dc, b1, "确认", ACT_CONFIRM_YES, 1);
            draw_pill(a, dc, b3, "取消", ACT_CONFIRM_NO, 0);
            break;
        case FORM_UPDATE: {
            RECT b4 = {panel.left + 284, fy, panel.left + 404, fy + 36};
            draw_pill(a, dc, b1, "立即更新", ACT_UPDATE_NOW, 1);
            draw_pill(a, dc, b2, "稍后", ACT_UPDATE_LATER, 0);
            draw_pill(a, dc, b4, "更新自检", ACT_UPDATE_TEST, 0);
            draw_pill(a, dc, b3, "关闭", ACT_OVERLAY_CLOSE, 0);
            break;
        }
        case FORM_DATA: {
            RECT b4 = {panel.left + 284, fy, panel.left + 404, fy + 36};
            draw_pill(a, dc, b1, "立即备份", ACT_BACKUP_NOW, 1);
            draw_pill(a, dc, b2, "恢复备份", ACT_BACKUP_RESTORE, 0);
            draw_pill(a, dc, b4, "清理旧备份", ACT_BACKUP_CLEAN, 0);
            draw_pill(a, dc, b3, "关闭", ACT_OVERLAY_CLOSE, 0);
            break;
        }
        case FORM_PLUGIN:
            draw_pill(a, dc, b1, "导入插件", ACT_PLUGIN_IMPORT_ACT, 1);
            draw_pill(a, dc, b2, "扩展点面板", ACT_PLUGIN_PANEL_CALL, 0);
            draw_pill(a, dc, b3, "关闭", ACT_OVERLAY_CLOSE, 0);
            break;
        case FORM_PLUGIN_IMPORT:
            draw_pill(a, dc, b1, a->plugin_pending_overwrite ? "覆盖安装" : "安装",
                      ACT_PLUGIN_INSTALL_CONFIRM, 1);
            draw_pill(a, dc, b3, "取消", ACT_OVERLAY_CLOSE, 0);
            break;
        case FORM_MSG: {
            RECT b4 = {panel.left + 284, fy, panel.left + 404, fy + 36};
            draw_pill(a, dc, b1, "复制", ACT_MSG_COPY, 0);
            draw_pill(a, dc, b2, "撤回", ACT_MSG_RECALL, 1);
            draw_pill(a, dc, b4, "修改", ACT_MSG_EDIT, 0);
            draw_pill(a, dc, b3, "关闭", ACT_OVERLAY_CLOSE, 0);
            break;
        }
        default:
            draw_pill(a, dc, b3, "关闭", ACT_OVERLAY_CLOSE, 1);
            break;
        }
    }
}

/* ---------- 顶层绘制 ---------- */
static void paint(App *a, HDC dc, RECT client)
{
    /* 动效：模式切换与三栏切换只做位移淡入，不重排内容。 */
    int dx = 0;
    long long now = aic_now_ms();
    RECT content;

    if (a->anim_on) {
        long long el = now - a->anim_start;
        if (el >= a->anim_dur) {
            a->anim_on = 0;
        } else {
            double t = (double)el / (double)a->anim_dur;
            dx = (int)((1.0 - t) * 10.0);
            a->caret_on = 1;
        }
    }
    a->caret_on = ((now / 500) % 2) == 0;

    {
        HBRUSH b = CreateSolidBrush(tok(a, AIC_TOK_PAGE_BG));
        FillRect(dc, &client, b);
        DeleteObject(b);
    }
    hit_clear();

    /* FR-30：首次启动先走 API Key 配置，再选默认模式，之后才出现三栏主界面。 */
    if (aic_router_state(a->router) == AIC_ST_CREDENTIAL_CHECK ||
        aic_router_state(a->router) == AIC_ST_CREDENTIAL_SETUP) {
        paint_credential(a, dc, client);
        paint_overlay(a, dc, client);
        return;
    }
    if (aic_router_state(a->router) == AIC_ST_MODE_SELECT) {
        paint_mode_select(a, dc, client);
        paint_overlay(a, dc, client);
        return;
    }
    if (aic_router_state(a->router) == AIC_ST_FIRST_RUN ||
        aic_router_state(a->router) == AIC_ST_WIZARD) {
        RECT t = {client.left + 40, client.top + 80, client.right - 40, client.top + 130};
        RECT d = {client.left + 40, client.top + 130, client.right - 40, client.top + 260};
        RECT b1 = {client.left + 40, client.top + 280, client.left + 220, client.top + 320};
        RECT b2 = {client.left + 236, client.top + 280, client.left + 396, client.top + 320};
        draw_text(dc, a->font_title, "欢迎使用 Sai", t, tok(a, AIC_TOK_TEXT),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, a->font_ui,
                  "首次启动需要做几件事：选择默认模式、填入 DeepSeek API Key、"
                  "设置一个陪伴人设。这些都可以稍后在设置里改，新手引导随时能重看。",
                  d, tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
        draw_pill(a, dc, b1, "开始设置", ACT_ROW_KEY, 1);
        draw_pill(a, dc, b2, "先看看", ACT_GUIDE_VIEW, 0);
        paint_overlay(a, dc, client);
        return;
    }

    content.left = client.left + NAV_W + dx;
    content.top = client.top;
    content.right = client.right;
    content.bottom = client.bottom - 26;

    paint_nav(a, dc, client);

    {
        /* 内容区裁剪：超出可视区域的滚动内容不会溢到导航栏或状态栏上。 */
        int saved = SaveDC(dc);
        if (saved) IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
        switch (aic_router_state(a->router)) {
        case AIC_ST_QA_READY:
        case AIC_ST_QA_LIST:
        case AIC_ST_QA_THREAD:
            paint_qa(a, dc, content);
            break;
        case AIC_ST_CHAT_TAB:
        case AIC_ST_CHAT_READY:
            switch (aic_router_tab(a->router)) {
            case AIC_TAB_ME:       paint_me(a, dc, content); break;
            case AIC_TAB_SETTINGS: paint_settings(a, dc, content); break;
            default:               paint_chat(a, dc, content); break;
            }
            break;
        default:
            paint_chat(a, dc, content);
            break;
        }
        if (saved) RestoreDC(dc, saved);
    }

    /* 状态栏 */
    if (a->status[0] && now < a->status_until) {
        RECT sb = {client.left + NAV_W, client.bottom - 24, client.right, client.bottom};
        HBRUSH b = CreateSolidBrush(tok(a, AIC_TOK_CARD_BG));
        FillRect(dc, &sb, b);
        DeleteObject(b);
        draw_text(dc, a->font_small, a->status,
                  (RECT){sb.left + 16, sb.top, sb.right - 16, sb.bottom},
                  tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* Toast */
    if (a->toast[0] && now < a->toast_until) {
        int tw = 320;
        RECT r = {client.right - tw - 24, client.bottom - 110, client.right - 24, client.bottom - 70};
        fill_round(dc, r, 10, tok(a, AIC_TOK_PRIMARY));
        draw_text(dc, a->font_ui, a->toast, r, RGB(255, 255, 255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    paint_overlay(a, dc, client);
}

/* ---------- 表单编辑辅助 ---------- */
static void edit_backspace(char *buf, size_t cap)
{
    size_t n = strlen(buf);
    (void)cap;
    if (!n) return;
    while (n > 1 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    buf[n] = '\0';
}

static void edit_append(char *buf, size_t cap, const wchar_t *w, int n)
{
    char mb[16];
    int k;
    size_t len;
    if (!w || n <= 0) return;
    k = WideCharToMultiByte(CP_UTF8, 0, w, n, mb, (int)sizeof(mb) - 1, NULL, NULL);
    if (k <= 0) return;
    mb[k] = '\0';
    len = strlen(buf);
    if (len + (size_t)k + 1 > cap) return;
    memcpy(buf + len, mb, (size_t)k);
    buf[len + (size_t)k] = '\0';
}

static int pick_file(HWND hwnd, const wchar_t *filter, wchar_t *out, int cap)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    out[0] = 0;
    return GetOpenFileNameW(&ofn) ? 1 : 0;
}

/* ---------- 动作实现 ---------- */
/* ---------- 后台发送：工作线程 + 消息队列（技术设计 §4.2） ---------- */
#define WM_APP_DELTA (WM_APP + 11)
#define WM_APP_DONE  (WM_APP + 12)

typedef struct ResultMsg {
    int                rc;
    char               err[256];
    aic_engine_result  res;
} ResultMsg;

typedef struct SendJob {
    char text[4096];
} SendJob;

static void on_stream_delta(void *ud, const char *delta)
{
    App *a = (App *)ud;
    if (!delta || !delta[0]) return;
    char *copy = aic_strdup(delta);
    if (!copy) return;
    if (!PostMessageA(a->hwnd, WM_APP_DELTA, 0, (LPARAM)copy)) aic_xfree(copy);
}

static DWORD WINAPI send_worker(LPVOID param)
{
    App *a = &g_app;
    SendJob *job = (SendJob *)param;
    ResultMsg *rm = (ResultMsg *)aic_xcalloc(1, sizeof(ResultMsg));
    rm->rc = aic_engine_send(a->eng, a->conv_id, job->text, &rm->res, rm->err, sizeof(rm->err));
    aic_xfree(job);
    PostMessageA(a->hwnd, WM_APP_DONE, 0, (LPARAM)rm);
    return 0;
}

static int app_start_worker(App *a, const char *text)
{
    if (InterlockedExchange(&a->sending, 1) != 0) {
        app_status(a, "上一条还在生成，请先停止或稍候");
        return AIC_E_BUSY;
    }
    SendJob *job = (SendJob *)aic_xcalloc(1, sizeof(SendJob));
    aic_strlcpy_utf8(job->text, sizeof(job->text), text);
    a->stream_len = 0;
    a->stream_text[0] = '\0';
    HANDLE th = CreateThread(NULL, 0, send_worker, job, 0, NULL);
    if (!th) {
        InterlockedExchange(&a->sending, 0);
        aic_xfree(job);
        app_status(a, "无法创建发送线程");
        return AIC_E_STATE;
    }
    CloseHandle(th);
    return AIC_OK;
}

/* 简易 Markdown：去掉代码围栏标记与 ** / 反引号，保留纯文本（代码块用等宽字体单独画）。 */
static void md_strip(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *q = in ? in : ""; *q && n + 1 < cap; q++) {
        if (q[0] == '*' && q[1] == '*') { q++; continue; }
        if (*q == '`') continue;
        out[n++] = *q;
    }
    out[n] = '\0';
}

static void app_send(App *a)
{
    char text[4096];

    if (!a->eng || !a->conv_id[0]) {
        app_status(a, "当前没有可用的会话");
        return;
    }
    if (a->draft_len <= 0) {
        app_status(a, "请先输入内容");
        return;
    }
    aic_strlcpy_utf8(text, sizeof(text), a->draft);
    a->draft[0] = '\0';
    a->draft_len = 0;
    if (app_start_worker(a, text) != AIC_OK) {
        aic_strlcpy_utf8(a->draft, sizeof(a->draft), text);
        a->draft_len = (int)strlen(a->draft);
    } else {
        a->scroll[2] = 0;
        app_status(a, "正在生成，可点「停止」中断");
    }
    app_invalidate(a);
}

static DWORD WINAPI regen_worker(LPVOID param)
{
    App *a = &g_app;
    ResultMsg *rm = (ResultMsg *)aic_xcalloc(1, sizeof(ResultMsg));
    char old_id[40];
    rm->rc = aic_engine_regenerate(a->eng, a->conv_id, old_id, sizeof(old_id), &rm->res,
                                   rm->err, sizeof(rm->err));
    (void)param;
    PostMessageA(a->hwnd, WM_APP_DONE, 0, (LPARAM)rm);
    return 0;
}

static void app_regenerate(App *a)
{
    if (!a->eng || !a->conv_id[0]) return;
    if (InterlockedCompareExchange(&a->sending, 1, 0) != 0) {
        app_status(a, "正在生成中，请先停止");
        return;
    }
    a->stream_len = 0;
    a->stream_text[0] = '\0';
    HANDLE th = CreateThread(NULL, 0, regen_worker, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else {
        InterlockedExchange(&a->sending, 0);
        app_status(a, "无法创建线程");
        return;
    }
    app_status(a, "正在重新生成…");
    app_invalidate(a);
}

static void app_on_done(App *a, ResultMsg *rm)
{
    InterlockedExchange(&a->sending, 0);
    if (!rm) return;
    if (rm->rc == AIC_OK) {
        app_load_chat(a);
        a->scroll[2] = 0;
        app_status(a, "已收到回复：输入 %lld / 输出 %lld tokens",
                   (long long)rm->res.prompt_tokens, (long long)rm->res.completion_tokens);
        aic_guide_mark(a->db, "first_chat", 0);
        /* 新消息是本端产生的实体变更，交给发件箱（§16.4 / FR-27）。 */
        app_publish(a, AIC_ENT_MESSAGE, AIC_OP_CREATE, rm->res.user_msg_id);
        app_publish(a, AIC_ENT_MESSAGE, AIC_OP_CREATE, rm->res.assistant_msg_id);
    } else if (rm->rc == AIC_E_TIMEOUT || rm->rc == AIC_E_BUSY) {
        app_status(a, "生成已停止");
        app_load_chat(a);
    } else if (rm->rc == AIC_E_AUTH) {
        app_status(a, "发送失败：%s", rm->err[0] ? rm->err : "API Key 不可用");
        app_toast(a, "请先在设置里配置 API Key");
    } else {
        app_status(a, "发送失败：%s", rm->err[0] ? rm->err : aic_strerror(rm->rc));
    }
    aic_engine_result_free(&rm->res);
    aic_xfree(rm);
    a->stream_len = 0;
    a->stream_text[0] = '\0';
    app_invalidate(a);
}

/* ---------- 导出与导入放在工作线程（§4.2：界面线程不做批量 IO） ---------- */
#define WM_APP_IO (WM_APP + 15)

typedef struct IoMsg {
    int kind;   /* 0=JSON 1=Markdown 2=纯文本 3=导入 */
    aic_export_scope sc;   /* 导出范围与是否含头像（技术设计 8.1） */
    int rc;
    char path[900];
    char err[512];
    aic_import_preview pv;
    aic_import_result rs;
} IoMsg;

static DWORD WINAPI io_worker(LPVOID param)
{
    IoMsg *m = (IoMsg *)param;
    App *a = &g_app;

    if (m->kind == 3) {
        memset(&m->pv, 0, sizeof(m->pv));
        memset(&m->rs, 0, sizeof(m->rs));
        m->rc = aic_transfer_preview(a->db, m->path, &m->pv, m->err, sizeof(m->err));
        if (m->rc == AIC_OK)
            m->rc = aic_transfer_import(a->db, m->path, AIC_CONFLICT_SKIP, &m->rs,
                                        m->err, sizeof(m->err));
    } else if (m->kind == 1) {
        m->rc = aic_export_write_markdown_ex(a->db, -1, &m->sc, m->path, m->err, sizeof(m->err));
    } else if (m->kind == 2) {
        m->rc = aic_export_write_text_ex(a->db, -1, &m->sc, m->path, m->err, sizeof(m->err));
    } else {
        m->rc = aic_export_write_json_ex(a->db, -1, &m->sc, m->path, m->err, sizeof(m->err));
    }
    PostMessageA(a->hwnd, WM_APP_IO, 0, (LPARAM)m);
    return 0;
}

static int app_start_io(App *a, IoMsg *m)
{
    HANDLE th = CreateThread(NULL, 0, io_worker, m, 0, NULL);
    if (!th) {
        aic_xfree(m);
        app_status(a, "无法创建线程，操作已取消");
        return AIC_E_NOMEM;
    }
    CloseHandle(th);
    return AIC_OK;
}

static void app_on_io(App *a, IoMsg *m)
{
    if (!m) return;
    if (m->rc != AIC_OK) {
        if (m->kind == 3)
            app_status(a, "导入失败（已回滚）：%s", m->err[0] ? m->err : aic_strerror(m->rc));
        else
            app_status(a, "导出失败：%s", m->err[0] ? m->err : aic_strerror(m->rc));
        app_toast(a, m->kind == 3 ? "导入未完成" : "导出未完成");
        aic_xfree(m);
        app_invalidate(a);
        return;
    }
    if (m->kind == 3) {
        app_reload_threads(a);
        app_load_chat(a);
        aic_guide_mark(a->db, "record_transfer", 0);
        app_set_overlay(a, AIC_OV_CONFIRM, "导入完成");
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "文件内的会话 %d 个、消息 %d 条；冲突会话 %d 个、冲突消息 %d 条。\n"
                 "本次结果：会话新增 %d / 覆盖 %d / 跳过 %d / 改名 %d，"
                 "消息新增 %d / 覆盖 %d / 跳过 %d / 改名 %d。",
                 (int)m->pv.conversations, (int)m->pv.messages,
                 (int)m->pv.conflict_conversations, (int)m->pv.conflict_messages,
                 (int)m->rs.conv_added, (int)m->rs.conv_overwritten, (int)m->rs.conv_skipped,
                 (int)m->rs.conv_renamed,
                 (int)m->rs.msg_added, (int)m->rs.msg_overwritten, (int)m->rs.msg_skipped,
                 (int)m->rs.msg_renamed);
        app_status(a, "导入完成：新增 %d 个会话 / %d 条消息",
                   (int)m->rs.conv_added, (int)m->rs.msg_added);
    } else {
        aic_guide_mark(a->db, "record_transfer", 0);
        app_status(a, "导出成功：%s", m->path);
        app_toast(a, "已导出到 exports 目录");
        app_close_overlay(a);
    }
    aic_xfree(m);
    app_invalidate(a);
}

static void app_export(App *a, int kind)
{
    char dir[600];
    char name[160];
    char ts[32];
    IoMsg *m;

    aic_path_join(dir, sizeof(dir), a->root, "exports");
    aic_mkdir_p(dir);
    aic_now_iso8601(ts);
    for (char *q = ts; *q; q++)
        if (*q == ':' || *q == '.') *q = '-';
    snprintf(name, sizeof(name), "Sai_%s_%s_v1.%s",
             aic_router_mode(a->router) == AIC_MODE_QA ? "qa" : "chat", ts,
             kind == 0 ? "json" : (kind == 1 ? "md" : "txt"));

    m = (IoMsg *)aic_xcalloc(1, sizeof(IoMsg));
    m->kind = kind;
    m->sc.scope = a->export_scope;
    m->sc.conv_id = a->conv_id;
    m->sc.include_avatars = a->export_avatars;
    m->sc.root = a->root;
    if (a->export_scope == 2) {
        m->sc.to_ms = aic_now_ms();
        m->sc.from_ms = m->sc.to_ms - 7LL * 24 * 3600 * 1000;
    }
    aic_path_join(m->path, sizeof(m->path), dir, name);
    if (app_start_io(a, m) != AIC_OK) return;
    app_status(a, "正在导出（后台线程）…");
    app_invalidate(a);
}

static void app_import(App *a)
{
    wchar_t wf[1024];
    IoMsg *m;

    if (!pick_file(a->hwnd, L"记录文件 (*.json)\0*.json\0所有文件\0*.*\0\0", wf, 1024)) return;
    m = (IoMsg *)aic_xcalloc(1, sizeof(IoMsg));
    m->kind = 3;
    w_to_u8(wf, m->path, sizeof(m->path));
    if (app_start_io(a, m) != AIC_OK) return;
    app_status(a, "正在导入（后台线程，失败自动回滚）…");
    app_invalidate(a);
}

/* ---------- 系统主题（§7.3 跟随系统） ---------- */
static int read_system_night(void)
{
    HKEY k;
    DWORD v = 1, sz = sizeof(v), type = 0;
    int night = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
            night = (v == 0);
        RegCloseKey(k);
    }
    return night;
}

/* ---------- 头像（§7.1 选择图片 → 最长边 512px → 存 avatars/） ---------- */
static void app_avatar_pick(App *a)
{
    wchar_t wf[1024];
    char path[2048];
    char rel[256];
    char err[512] = {0};
    aic_profile pr;

    if (!aic_avatar_available()) {
        app_status(a, "当前环境不支持图片处理（GDI+ 不可用）");
        return;
    }
    if (!pick_file(a->hwnd, L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0\0",
                   wf, 1024))
        return;
    w_to_u8(wf, path, sizeof(path));
    if (aic_avatar_import(path, a->root, rel, sizeof(rel), err, sizeof(err)) != AIC_OK) {
        app_status(a, "头像导入失败：%s", err[0] ? err : "未知错误");
        return;
    }
    memset(&pr, 0, sizeof(pr));
    aic_profile_get(a->db, &pr);
    aic_strlcpy_utf8(pr.avatar_path, sizeof(pr.avatar_path), rel);
    if (aic_profile_set(a->db, &pr, err, sizeof(err)) != AIC_OK) {
        app_status(a, "保存头像路径失败：%s", err[0] ? err : "未知错误");
        return;
    }
    app_status(a, "头像已更新（最长边 512px，已压缩存到 avatars/）");
    app_invalidate(a);
}

/* ---------- 余额（FR-11：账户接口 + 本地账本并列） ---------- */
#define WM_APP_UPD (WM_APP + 13)
#define WM_APP_BAL (WM_APP + 14)

typedef struct TextMsg {
    int  rc;
    char text[768];
} TextMsg;

static void balance_endpoint(App *a, char *out, size_t cap)
{
    char url[AIC_LLM_URL_MAX];
    if (aic_setting_get(a->db, AIC_SET_CRED_ENDPOINT, url, sizeof(url)) != AIC_OK || !url[0])
        aic_strlcpy_utf8(url, sizeof(url), AIC_LLM_MODEL_DEFAULT_URL);
    const char *slash = strstr(url, "/chat/");
    if (slash) {
        size_t base = (size_t)(slash - url);
        if (base + 13 >= cap) base = cap - 14;
        memcpy(out, url, base);
        aic_strlcpy_utf8(out + base, cap - base, "/user/balance");
    } else {
        snprintf(out, cap, "https://api.deepseek.com/user/balance");
    }
}

static DWORD WINAPI bal_worker(LPVOID param)
{
    App *a = &g_app;
    TextMsg *tm = (TextMsg *)aic_xcalloc(1, sizeof(TextMsg));
    char url[512];
    char key[512];
    char err[256] = {0};
    char *body = NULL;
    int status = 0;
    (void)param;

    balance_endpoint(a, url, sizeof(url));
    key[0] = '\0';
    if (aic_cred_load(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, key, sizeof(key), err,
                      sizeof(err)) != AIC_OK || !key[0]) {
        tm->rc = AIC_E_AUTH;
        aic_strlcpy_utf8(tm->text, sizeof(tm->text), "尚未配置 API Key");
    } else {
        tm->rc = aic_http_get_json(url, key, &body, &status, err, sizeof(err));
        if (tm->rc == AIC_OK && body) {
            aic_llm_balance bal;
            char perr[128] = {0};
            memset(&bal, 0, sizeof(bal));
            if (aic_llm_balance_parse(body, strlen(body), &bal, perr, sizeof(perr)) == AIC_OK) {
                char cost[48];
                aic_llm_format_micros(bal.available_micros, cost, sizeof(cost));
                snprintf(tm->text, sizeof(tm->text),
                         "账户接口：可用余额 %s，未出账 %lld 微元；本地账本累计见用量面板。",
                         cost, (long long)bal.pending_micros);
            } else {
                tm->rc = AIC_E_FORMAT;
                aic_strlcpy_utf8(tm->text, sizeof(tm->text), perr);
            }
            if (body) aic_xfree(body);
        } else {
            snprintf(tm->text, sizeof(tm->text), "余额查询失败（HTTP %d）：%s", status, err);
        }
        aic_secure_zero(key, sizeof(key));
    }
    PostMessageA(a->hwnd, WM_APP_BAL, 0, (LPARAM)tm);
    return 0;
}

static void app_balance_refresh(App *a)
{
    HANDLE th = CreateThread(NULL, 0, bal_worker, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else app_status(a, "无法创建余额查询线程");
}

static void app_on_bal(App *a, TextMsg *tm)
{
    if (!tm) return;
    if (tm->rc == AIC_OK) {
        a->bal.queried_ms = aic_now_ms();
        aic_strlcpy_utf8(a->bal.source, sizeof(a->bal.source), "账户接口");
        a->bal.stale = 0;
        a->bal.valid = 1;
    } else {
        aic_llm_balance_keep_last(&a->bal, &a->bal, aic_now_ms());
        a->bal.valid = 0;
        aic_strlcpy_utf8(a->bal.source, sizeof(a->bal.source), "上次结果");
    }
    {
        aic_usage_summary sum;
        memset(&sum, 0, sizeof(sum));
        aic_usage_summary_get(a->db, 0, &sum);
        a->bal.local_used_micros = sum.cost_micros;
        if (a->bal.valid && aic_llm_balance_below(&a->bal,
                aic_setting_get_int(a->db, AIC_SET_BALANCE_ALERT, 0))) {
            a->bal.below_alert = 1;
            aic_notify(a->hwnd, "余额偏低", "账户余额已低于告警阈值，请到设置里查看用量");
        }
    }
    app_status(a, "%s", tm->text);
    app_toast(a, tm->text);
    aic_xfree(tm);
    app_invalidate(a);
}

/* ---------- 更新（§14.1：启动与周期检查、非阻塞提示、断点续传、校验、替换安装） ---------- */
static int upd_manifest_path(App *a, char *out, size_t cap)
{
    char dir[600];
    aic_path_join(dir, sizeof(dir), a->root, "updates");
    aic_mkdir_p(dir);
    aic_path_join(out, cap, dir, "manifest.json");
    return aic_path_exists(out) ? AIC_OK : AIC_E_NOTFOUND;
}

static void app_check_update(App *a, int user_initiated)
{
    char mf[900];
    char *data = NULL;
    size_t len = 0;
    aic_update_manifest m;
    aic_update_plan plan;
    char err[512] = {0};
    char skip[64];
    int rc;

    a->upd_last_check = aic_now_ms();
    aic_setting_set_int(a->db, "update.last_check_ms", (long long)a->upd_last_check);
    if (upd_manifest_path(a, mf, sizeof(mf)) != AIC_OK) {
        if (user_initiated) app_update(a); /* 没有清单：展示更新链路说明与数据保护规则 */
        return;
    }
    if (aic_file_read_all(mf, &data, &len) != AIC_OK) {
        if (user_initiated) app_status(a, "读取更新清单失败");
        return;
    }
    memset(&m, 0, sizeof(m));
    rc = aic_update_parse_manifest(data, len, &m, err, sizeof(err));
    aic_xfree(data);
    if (rc != AIC_OK) {
        if (user_initiated) app_status(a, "更新清单不可用：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    skip[0] = '\0';
    aic_setting_get(a->db, "update.skip_version", skip, sizeof(skip));
    memset(&plan, 0, sizeof(plan));
    aic_update_make_plan(&m, APP_VERSION, skip, &plan);
    if (plan.status == AIC_UPD_UP_TO_DATE || plan.status == AIC_UPD_SKIPPED) {
        if (user_initiated) app_status(a, "已是最新版本（%s）", APP_VERSION);
        return;
    }
    if (plan.status != AIC_UPD_AVAILABLE) {
        if (user_initiated) app_status(a, "当前版本过低，需要整包安装");
        return;
    }
    a->upd = m;
    a->upd_available = 1;
    app_set_overlay(a, AIC_OV_UPDATE, "发现新版本");
    a->confirm_action = FORM_UPDATE;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "当前版本 %s，可用版本 %s（渠道 %s，发布于 %s）%s。\n\n"
             "安装包：%s（%lld 字节）\nSHA-256：%s\n\n更新说明：%s\n\n"
             "更新前会做一致性检查与目录外完整备份；备份成功才会开始写入，失败自动回滚。",
             APP_VERSION, m.version, m.channel[0] ? m.channel : "stable", m.released_at,
             plan.mandatory ? "（强制更新）" : "", m.url, m.size, m.sha256,
             m.notes[0] ? m.notes : "（无）");
    if (!a->notified_update) {
        a->notified_update = 1;
        aic_notify(a->hwnd, "Sai 有新版本", m.version);
    }
}

/* 把 aic_http_download 适配成更新模块要求的下载回调（断点续传由 resume=1 打开）。 */
static int dl_http(void *ud, const char *url, const char *dst, long long max, char *err,
                   size_t errlen)
{
    (void)ud;
    return aic_http_download(url, dst, max, 1, NULL, NULL, NULL, err, errlen);
}

static DWORD WINAPI upd_worker(LPVOID param)
{
    App *a = &g_app;
    TextMsg *tm = (TextMsg *)aic_xcalloc(1, sizeof(TextMsg));
    char dir[600];
    char bdir[600];
    char dst[900];
    char appdir[700];
    char exe[700];
    char err[512] = {0};
    aic_update_manifest m = a->upd;
    (void)param;

    aic_path_join(dir, sizeof(dir), a->root, "updates");
    aic_mkdir_p(dir);
    aic_path_join(bdir, sizeof(bdir), a->root, "backups");
    aic_mkdir_p(bdir);
    aic_path_join(dst, sizeof(dst), dir, "pkg.download");

    GetModuleFileNameA(NULL, exe, sizeof(exe));
    aic_strlcpy_utf8(appdir, sizeof(appdir), exe);
    {
        char *bar = strrchr(appdir, '\\');
        if (bar) *bar = '\0';
    }
    app_status(a, "正在下载更新包…");
    tm->rc = aic_update_run_install(&m, dst, dl_http, NULL, 1, appdir, bdir, NULL,
                                    err, sizeof(err));
    if (tm->rc == AIC_OK)
        snprintf(tm->text, sizeof(tm->text), "更新完成：%s，请重新启动 Sai。", m.version);
    else
        snprintf(tm->text, sizeof(tm->text), "更新未完成：%s", err[0] ? err : aic_strerror(tm->rc));
    PostMessageA(a->hwnd, WM_APP_UPD, 0, (LPARAM)tm);
    return 0;
}

static void app_update_now(App *a)
{
    HANDLE th;
    if (!a->upd_available) {
        app_status(a, "没有可用的更新包");
        return;
    }
    th = CreateThread(NULL, 0, upd_worker, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else app_status(a, "无法创建更新线程");
}

static void app_on_upd(App *a, TextMsg *tm)
{
    if (!tm) return;
    app_status(a, "%s", tm->text);
    app_toast(a, tm->text);
    if (tm->rc == AIC_OK) a->upd_available = 0;
    aic_xfree(tm);
    app_invalidate(a);
}

/* ---------- 备份与清理（§8.3 数据管理） ---------- */
static void app_backup_now(App *a)
{
    char bdir[700];
    char zip[900];
    char dbp[800];
    char ts[32];
    aic_backup_info info;
    char err[512] = {0};
    int rc;

    aic_path_join(bdir, sizeof(bdir), a->root, "backups");
    aic_mkdir_p(bdir);
    aic_now_iso8601(ts);
    snprintf(zip, sizeof(zip), "%s\\sai-backup-%s.zip", bdir, ts);
    {
        char *q = zip + strlen(bdir) + 1;
        for (char *r = q; *r; r++)
            if (*r == ':' || *r == '.') *r = '-';
    }
    aic_path_join(dbp, sizeof(dbp), a->root, "db\\aic.db");
    memset(&info, 0, sizeof(info));
    rc = aic_backup_create(a->root, dbp, APP_VERSION, zip, &info, err, sizeof(err));
    if (rc != AIC_OK) {
        app_status(a, "备份失败：%s", err[0] ? err : aic_strerror(rc));
        aic_log_error("backup_create", rc, err);
        return;
    }
    app_status(a, "备份完成：%s（%d 个文件，%lld 字节）", zip, info.file_count,
               (long long)info.total_bytes);
    app_begin_anim(a, 3, aic_motion_default().overlay_ms);
}

static void app_backup_restore(App *a)
{
    char bdir[700];
    char zip[900];
    aic_backup_info info;
    char err[512] = {0};
    int rc;

    aic_path_join(bdir, sizeof(bdir), a->root, "backups");
    zip[0] = '\0';
    if (aic_backup_latest(bdir, zip, sizeof(zip), &info, err, sizeof(err)) != AIC_OK) {
        app_status(a, "没有可恢复的备份：%s", err[0] ? err : "backups/ 为空");
        return;
    }
    rc = aic_backup_restore(zip, a->root, err, sizeof(err));
    if (rc != AIC_OK) {
        app_status(a, "恢复失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    app_load_chat(a);
    app_reload_threads(a);
    app_status(a, "已从备份恢复（%s），重启后生效更完整", zip);
}

static void app_backup_clean(App *a)
{
    char bdir[700];
    int removed = 0;
    char err[512] = {0};
    aic_path_join(bdir, sizeof(bdir), a->root, "backups");
    if (aic_backup_clean(bdir, 3, &removed, err, sizeof(err)) != AIC_OK) {
        app_status(a, "清理失败：%s", err[0] ? err : "未知错误");
        return;
    }
    app_status(a, "已清理 %d 个旧备份（保留最近 3 个）", removed);
}

/* ---------- 插件面板（§10：安装/启用/禁用/更新） ---------- */
static void app_plugin_panel(App *a)
{
    char root[700];
    aic_plugin *arr = NULL;
    size_t n = 0;

    aic_path_join(root, sizeof(root), a->root, "plugins");
    aic_mkdir_p(root);
    if (aic_plugin_list(a->db, -1, &arr, &n) != AIC_OK) n = 0;
    if (a->plugs) aic_plugin_free_array(a->plugs, a->plug_n);
    a->plugs = arr;
    a->plug_n = n;

    app_set_overlay(a, AIC_OV_CONFIRM, "插件");
    a->confirm_action = FORM_PLUGIN;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "已安装 %d 个插件，启用 %d 个。插件跑在独立进程里，超时或无响应会被宿主终止并禁用；"
             "插件数据目录与核心目录分离，卸载时可选择保留。"
             "扩展点：prompt.fragment（提示词片段）、ui.panel（面板按钮）、message.transform（请求改写）、"
             "tool.provider（工具提供）。",
             (int)n, (int)(n > 0 ? 1 : 0));
    a->field_n = 0;
}

/* 调用 ui.panel 扩展点：所有启用且声明了 ui.panel 的插件都会被问一遍 describe。 */
static void app_plugin_panel_call(App *a)
{
    char exe[900];
    char dir[900];
    char host[1000];
    char err[512] = {0};
    int rc;

    GetModuleFileNameA(NULL, exe, sizeof(exe));
    aic_strlcpy_utf8(dir, sizeof(dir), exe);
    {
        char *bar = strrchr(dir, '\\');
        if (bar) *bar = '\0';
    }
    aic_path_join(host, sizeof(host), dir, "aic_plugin_host.exe");
    a->plugin_panel[0] = '\0';
    rc = aic_plugin_call_point(a->db, host, "ui.panel", "describe", "{}", a->plugin_panel,
                               sizeof(a->plugin_panel), err, sizeof(err));
    a->plugin_panel_n = 1;
    if (rc != AIC_OK) {
        snprintf(a->overlay_text, sizeof(a->overlay_text), "扩展点调用失败：%s",
                 err[0] ? err : aic_strerror(rc));
        app_status(a, "扩展点调用失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "ui.panel 的 describe 结果（没有插件声明该扩展点时 count 为 0）：\n%s",
             a->plugin_panel);
    app_status(a, "已调用 ui.panel 扩展点");
}

static void app_plugin_toggle(App *a, size_t idx)
{
    char err[512] = {0};
    int rc;
    if (idx >= a->plug_n) return;
    rc = aic_plugin_set_enabled(a->db, a->plugs[idx].id, a->plugs[idx].enabled ? 0 : 1, err,
                                sizeof(err));
    if (rc != AIC_OK) {
        app_status(a, "切换插件状态失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    app_publish(a, AIC_ENT_PLUGIN, AIC_OP_UPDATE, a->plugs[idx].id);
    app_plugin_panel(a);
    app_status(a, "已切换插件「%s」", a->plugs[idx].name);
}

static void app_plugin_update(App *a, size_t idx)
{
    wchar_t wf[1024];
    char path[2048];
    char root[700];
    aic_plugin plg;
    char err[512] = {0};
    int rc;

    if (idx >= a->plug_n) return;
    if (!pick_file(a->hwnd, L"插件清单 (*.json)\0*.json\0所有文件\0*.*\0\0", wf, 1024)) return;
    w_to_u8(wf, path, sizeof(path));
    aic_path_join(root, sizeof(root), a->root, "plugins");
    memset(&plg, 0, sizeof(plg));
    rc = aic_plugin_update(a->db, root, path, &plg, err, sizeof(err));
    if (rc != AIC_OK) {
        app_status(a, "插件更新失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    app_status(a, "插件「%s」已更新到 %s", plg.name, plg.version);
    aic_plugin_free(&plg);
    app_plugin_panel(a);
}

/* 技术设计 10.2：安装前先把来源、版本、权限摊开给用户确认，不静默安装。 */
static void app_plugin_import(App *a)
{
    wchar_t wf[1024];
    char path[2048];
    char *text = NULL;
    size_t len = 0;
    aic_plugin_manifest mf;
    char err[512] = {0};
    aic_plugin exist;

    if (!pick_file(a->hwnd, L"插件清单 (*.json)\0*.json\0所有文件\0*.*\0\0", wf, 1024)) return;
    w_to_u8(wf, path, sizeof(path));
    if (aic_file_read_all(path, &text, &len) != AIC_OK || !text) {
        app_status(a, "读取插件清单失败：%s", path);
        return;
    }
    memset(&mf, 0, sizeof(mf));
    if (aic_plugin_parse_manifest(text, len, &mf, err, sizeof(err)) != AIC_OK) {
        aic_xfree(text);
        app_status(a, "插件清单不合法：%s", err[0] ? err : "格式错误");
        return;
    }
    aic_xfree(text);
    aic_strlcpy_utf8(a->plugin_pending, sizeof(a->plugin_pending), path);
    a->plugin_pending_overwrite = 0;
    memset(&exist, 0, sizeof(exist));
    if (aic_plugin_get(a->db, mf.id, &exist) == AIC_OK) a->plugin_pending_overwrite = 1;

    app_set_overlay(a, AIC_OV_CONFIRM, "安装插件");
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "来源：本地文件（%s）\n"
             "名称：%s\n"
             "标识：%s\n"
             "版本：%s\n"
             "作者：%s\n"
             "权限：%s\n"
             "扩展点：%s\n"
             "入口：%s\n"
             "入口哈希：%s\n\n"
             "插件跑在独立子进程里，超时或无响应会被终止并禁用；插件数据与核心数据目录分离。%s",
             path, mf.name, mf.id, mf.version, mf.author[0] ? mf.author : "未填写",
             mf.permissions[0] ? mf.permissions : "无",
             mf.extension_points[0] ? mf.extension_points : "无", mf.entry,
             mf.sha256[0] ? mf.sha256 : "清单未提供（跳过哈希校验）",
             a->plugin_pending_overwrite ? "\n\n注意：同名插件已安装，确认后覆盖更新。" : "");
    a->confirm_action = FORM_PLUGIN_IMPORT;
    app_invalidate(a);
}

/* 用户在确认浮层里点了「安装」之后才真正落地。 */
static void app_plugin_import_confirm(App *a)
{
    char root[700];
    aic_plugin plg;
    char err[512] = {0};
    int rc;

    if (!a->plugin_pending[0]) return;
    aic_path_join(root, sizeof(root), a->root, "plugins");
    memset(&plg, 0, sizeof(plg));
    rc = aic_plugin_install_ex(a->db, root, a->plugin_pending, "file",
                              a->plugin_pending_overwrite, &plg, err, sizeof(err));
    if (rc != AIC_OK) {
        app_status(a, "插件安装失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    aic_guide_mark(a->db, "plugin_import", 0);
    /* 只同步清单，插件二进制不参与同步（§16.4）。 */
    app_publish(a, AIC_ENT_PLUGIN, AIC_OP_CREATE, plg.id);
    app_status(a, "插件「%s」已安装（默认未启用）", plg.name);
    app_toast(a, "插件已安装，可在设置里启用");
    aic_plugin_free(&plg);
    a->plugin_pending[0] = '\0';
}

static void app_balance(App *a)
{
    aic_usage_summary sum;
    char cost[48];
    memset(&sum, 0, sizeof(sum));
    aic_usage_summary_get(a->db, 0, &sum);
    aic_llm_format_micros(sum.cost_micros, cost, sizeof(cost));

    app_set_overlay(a, AIC_OV_CONFIRM, "余额与用量");
    if (a->bal_valid) {
        char avail[48];
        aic_llm_format_micros(a->bal.available_micros, avail, sizeof(avail));
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "账户接口余额（来源：%s）：可用 %s"
                 "%s%s\n\n"
                 "本地账本（来源：usage_ledger）：%lld 次请求，输入 %lld / 输出 %lld tokens，"
                 "累计 %s。\n\n"
                 "两项并列展示并各自标注来源；账户接口查询失败时保留上次结果并标出过期时间；"
                 "低于阈值时通知，并在设置页显示横幅。",
                 a->bal.source[0] ? a->bal.source : "账户接口", avail,
                 a->bal.stale ? "（已过期，展示的是上次结果）" : "",
                 a->bal.pending_micros > 0 ? "，未出账另计" : "",
                 sum.requests, sum.prompt_tokens, sum.completion_tokens, cost);
    } else {
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "账户接口余额：还没有取到结果（未配置 API Key 或网络不可用）。\n\n"
                 "本地账本（来源：usage_ledger）：%lld 次请求，输入 %lld / 输出 %lld tokens，"
                 "累计 %s。\n\n"
                 "配置 Key 后重新打开本页会向账户接口查询一次；查询失败时保留上次结果并标出过期时间；"
                 "低于阈值时通知，并在设置页显示横幅。",
                 sum.requests, sum.prompt_tokens, sum.completion_tokens, cost);
    }
    aic_guide_mark(a->db, "balance_query", 0);
}

static void app_update(App *a)
{
    char dir[600];
    char mf[800];
    char *data = NULL;
    size_t len = 0;
    aic_update_manifest m;
    aic_update_plan plan;
    char err[512] = {0};
    int rc;

    aic_path_join(dir, sizeof(dir), a->root, "updates");
    aic_mkdir_p(dir);
    aic_path_join(mf, sizeof(mf), dir, "manifest.json");

    app_set_overlay(a, AIC_OV_UPDATE, "更新设置");
    a->confirm_action = FORM_UPDATE;

    if (!aic_path_exists(mf)) {
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "当前没有更新清单（%s 不存在）。\n\n"
                 "更新链路为：解析清单 → 版本比较 → 下载（可注入）→ 校验大小与 SHA-256 → "
                 "备份前置检查 → 平台层安装。没有配置下载通道时，任何下载都明确失败而不是假装成功。\n\n"
                 "数据保护：备份未成功时不允许开始写入；升级按 schema_version 顺序迁移，迁移前再备份一次，"
                 "失败自动回退；卸载默认保留用户数据。",
                 mf);
        return;
    }
    rc = aic_file_read_all(mf, &data, &len);
    if (rc != AIC_OK) {
        snprintf(a->overlay_text, sizeof(a->overlay_text), "读取更新清单失败：%s",
                 aic_strerror(rc));
        return;
    }
    memset(&m, 0, sizeof(m));
    rc = aic_update_parse_manifest(data, len, &m, err, sizeof(err));
    aic_xfree(data);
    if (rc != AIC_OK) {
        snprintf(a->overlay_text, sizeof(a->overlay_text), "更新清单不可用：%s",
                 err[0] ? err : aic_strerror(rc));
        return;
    }
    memset(&plan, 0, sizeof(plan));
    aic_update_make_plan(&m, APP_VERSION, "", &plan);
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "当前版本 %s，清单版本 %s（渠道 %s，发布于 %s）。\n"
             "判定：%s%s\n安装包：%s（%lld 字节）\nSHA-256：%s\n\n更新说明：%s",
             APP_VERSION, m.version, m.channel[0] ? m.channel : "stable",
             m.released_at,
             plan.status == AIC_UPD_UP_TO_DATE ? "已是最新" :
             plan.status == AIC_UPD_AVAILABLE ? "有新版本可用" :
             plan.status == AIC_UPD_TOO_OLD ? "当前版本过低，需要整包安装" :
             plan.status == AIC_UPD_SKIPPED ? "该版本已被跳过" : "清单无效",
             plan.mandatory ? "（强制更新）" : "",
             m.url, m.size, m.sha256, m.notes);
}

static void app_update_test(App *a)
{
    char dir[600];
    char mf[800];
    char dst[800];
    char *data = NULL;
    size_t len = 0;
    aic_update_manifest m;
    char err[512] = {0};
    int rc;

    aic_path_join(dir, sizeof(dir), a->root, "updates");
    aic_path_join(mf, sizeof(mf), dir, "manifest.json");
    aic_path_join(dst, sizeof(dst), dir, "pkg.download");

    if (!aic_path_exists(mf)) {
        app_status(a, "没有更新清单，先放一份 %s", mf);
        return;
    }
    if (aic_file_read_all(mf, &data, &len) != AIC_OK) {
        app_status(a, "读取更新清单失败");
        return;
    }
    memset(&m, 0, sizeof(m));
    rc = aic_update_parse_manifest(data, len, &m, err, sizeof(err));
    aic_xfree(data);
    if (rc != AIC_OK) {
        app_status(a, "清单不可用：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    memset(err, 0, sizeof(err));
    rc = aic_update_run(&m, dst, aic_update_download_local_copy, NULL, 1, 1, err, sizeof(err));
    if (rc == AIC_OK) {
        app_status(a, "更新自检通过：下载与校验（大小 / SHA-256）一致，未执行安装");
    } else {
        app_status(a, "更新自检未通过：%s", err[0] ? err : aic_strerror(rc));
    }
}

static void app_data(App *a)
{
    char dbp[800];
    int64_t sz = 0;
    long long msgs = 0;
    aic_conversation *all = NULL;
    size_t all_n = 0;
    aic_plugin *plgs = NULL;
    size_t plg_n = 0;
    char human[64] = "未知";

    aic_path_join(dbp, sizeof(dbp), a->root, "db\\aic.db");
    if (aic_file_size(dbp, &sz) == AIC_OK)
        snprintf(human, sizeof(human), "%.2f MB", (double)sz / (1024.0 * 1024.0));
    aic_conv_list(a->db, -1, &all, &all_n);
    aic_plugin_list(a->db, -1, &plgs, &plg_n);
    {
        aic_conversation conv;
        memset(&conv, 0, sizeof(conv));
        if (aic_conv_get_chat(a->db, &conv) == AIC_OK) aic_msg_count(a->db, conv.id, &msgs);
    }

    app_set_overlay(a, AIC_OV_CONFIRM, "数据管理");
    a->confirm_action = FORM_DATA;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "数据目录：%s\n数据库：%s（%s）\n会话：%d 个（聊天 1 + 问答 %d）；"
             "本机插件 %d 个。\n\n备份：迁移前自动复制到 backups/；更新前置要求备份成功。\n"
             "导出包只含会话与消息，不含凭证；备份包含凭证引用与插件数据，二者格式与解析器互不兼容。\n"
             "卸载默认保留数据。",
             a->root, dbp, human, (int)all_n,
             (int)(all_n > 0 ? all_n - 1 : 0), (int)plg_n);
    aic_conv_free(all, all_n);
    if (plgs) aic_plugin_free_array(plgs, plg_n);
}

static void app_guide(App *a)
{
    app_set_overlay(a, AIC_OV_GUIDE, "新手指南（9 步）");
}

static void app_msg_open(App *a, int idx)
{
    aic_message *m;
    int window = aic_setting_get_int(a->db, AIC_SET_RECALL_WINDOW, AIC_RECALL_WINDOW_DEFAULT);
    int can = 0;

    if (idx < 0 || (size_t)idx >= a->msg_n) return;
    m = &a->msgs[idx];
    aic_strlcpy_utf8(a->sel_msg, sizeof(a->sel_msg), m->id);
    if (a->eng && strcmp(m->role, "assistant") == 0)
        can = aic_engine_can_recall(a->eng, m->id, window);

    app_set_overlay(a, AIC_OV_CONFIRM, "消息操作");
    a->confirm_action = FORM_MSG;
    {
        /* 预览只取一行，正文太长会盖住底部操作按钮。 */
        const char *raw = m->recalled ? AIC_RECALL_NOTICE : (m->content ? m->content : "");
        char one[72];
        size_t k = 0;
        while (raw[k] && k + 1 < sizeof(one) && raw[k] != '\n') {
            one[k] = raw[k];
            k++;
        }
        one[k] = '\0';
        aic_utf8_truncate_chars(one, 40);
        if (raw[k]) aic_strlcpy_utf8(one + strlen(one), sizeof(one) - strlen(one), "…");
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "角色：%s\n时间：%s\n状态：%s\n预览：%s",
                 strcmp(m->role, "user") == 0 ? "用户" : (strcmp(m->role, "system") == 0 ? "系统" : "助手"),
                 m->created_at,
                 m->recalled ? "已撤回" : (m->edited_at[0] ? "已修改" : "正常"),
                 one);
    }
    a->field_n = 0;
    if (!can && !m->recalled) {
        /* 超出时间窗：仍允许修改，但不提供撤回入口。 */
    }
}

static void app_profile_edit(App *a)
{
    aic_profile p;
    memset(&p, 0, sizeof(p));
    aic_profile_get(a->db, &p);
    app_set_overlay(a, AIC_OV_CONFIRM, "编辑个人信息");
    a->confirm_action = FORM_PROFILE;
    fld(a, "昵称（最长 40 个字符，写入 {user_name}）", p.name, 0, 0);
    fld(a, "性别（未设置 / 男 / 女 / 其他）", p.gender, 0, 0);
    fld(a, "生日（YYYY-MM-DD）", p.birthday, 0, 0);
    fld(a, "职业（最长 60 个字符）", p.occupation, 0, 0);
    fld(a, "补充说明（最长 500 个字符）", p.extra_note, 1, 0);
    fld(a, "头像路径（应用内相对路径，禁止外链）", p.avatar_path, 0, 0);
    a->field_focus = 0;
}

static void app_persona_edit(App *a)
{
    aic_persona per;
    memset(&per, 0, sizeof(per));
    aic_persona_ensure_default(a->db, &per);
    app_set_overlay(a, AIC_OV_AI_INFO, "AI 信息与人设");
    a->confirm_action = FORM_PERSONA;
    fld(a, "人设名称", per.name, 0, 0);
    fld(a, "一句话描述", per.description, 0, 0);
    fld(a, "人设提示词（可用 {user_name} 等变量）", per.base_prompt ? per.base_prompt : "", 1, 0);
    a->field_focus = 0;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "当前版本 v%d。改动提示词会生成新版本，旧版本保留可回溯。", per.current_version);
    aic_persona_free(&per);
}

static void app_key_edit(App *a)
{
    app_set_overlay(a, AIC_OV_CONFIRM, "API Key");
    a->confirm_action = FORM_KEY;
    fld(a, "DeepSeek API Key（只写入系统安全存储，不写数据库）", "", 0, 0);
    a->field_focus = 0;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "凭证保存在 Windows 凭据管理器（服务名 Sai，账户 api_key）；"
             "界面只显示「已配置 / 未配置」，日志与导出包都不含明文。\n"
             "当前状态：%s",
             (a->eng && aic_engine_has_key(a->eng)) ? "已配置" : "未配置");
}

static void app_new_thread(App *a)
{
    aic_persona per;
    aic_conversation conv;
    char model[64] = {0};
    char title[64];
    int rc;

    memset(&per, 0, sizeof(per));
    aic_persona_ensure_default(a->db, &per);
    aic_setting_get(a->db, AIC_SET_MODEL_DEFAULT, model, sizeof(model));
    if (!model[0]) aic_strlcpy_utf8(model, sizeof(model), AIC_MODEL_FLASH);
    {
        char ts[32];
        aic_now_iso8601(ts);
        snprintf(title, sizeof(title), "新会话 %.13s", ts + 11);
    }

    memset(&conv, 0, sizeof(conv));
    rc = aic_conv_create(a->db, AIC_MODE_QA, title, per.id, model, &conv);
    aic_persona_free(&per);
    if (rc != AIC_OK) {
        app_status(a, "新建会话失败：%s", aic_strerror(rc));
        return;
    }
    aic_router_set_thread(a->router, conv.id);
    a->scroll[2] = 0;
    app_load_chat(a);
    app_reload_threads(a);
    app_status(a, "已新建问答会话「%s」", conv.title);
    app_publish(a, AIC_ENT_CONVERSATION, AIC_OP_CREATE, conv.id);
}

static void app_save_form(App *a)
{
    char err[512] = {0};
    int rc = AIC_OK;

    switch (a->confirm_action) {
    case FORM_PERSONA: {
        aic_persona cur;
        aic_persona out;
        memset(&cur, 0, sizeof(cur));
        memset(&out, 0, sizeof(out));
        aic_persona_ensure_default(a->db, &cur);
        rc = aic_persona_update(a->db, cur.id, a->fields[0].buf, a->fields[1].buf,
                                a->fields[2].buf, &out);
        if (rc == AIC_OK) {
            aic_strlcpy_utf8(a->persona_name, sizeof(a->persona_name), out.name);
            app_status(a, "人设已保存（版本 v%d）", out.current_version);
            aic_guide_mark(a->db, "persona_edit", 0);
            app_publish(a, AIC_ENT_PERSONA, AIC_OP_UPDATE, out.id);
            aic_persona_free(&out);
        }
        aic_persona_free(&cur);
        break;
    }
    case FORM_KEY: {
        const char *k = a->fields[0].buf;
        while (*k == ' ' || *k == '\t') k++;
        if (!*k) {
            rc = AIC_E_INVAL;
            aic_strlcpy_utf8(err, sizeof(err), "API Key 不能为空");
            break;
        }
        rc = aic_cred_save(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, k, err, sizeof(err));
        if (rc == AIC_OK) {
            aic_setting_set_int(a->db, AIC_SET_CRED_HAS_KEY, 1);
            aic_engine_set_api_key(a->eng, k);
            app_status(a, "API Key 已写入系统安全存储");
            aic_guide_mark(a->db, "api_key", 0);
        }
        aic_secure_zero(a->fields[0].buf, sizeof(a->fields[0].buf));
        break;
    }
    case FORM_PROFILE: {
        aic_profile p;
        memset(&p, 0, sizeof(p));
        aic_profile_get(a->db, &p);
        aic_strlcpy_utf8(p.name, sizeof(p.name), a->fields[0].buf);
        aic_strlcpy_utf8(p.gender, sizeof(p.gender), a->fields[1].buf);
        aic_strlcpy_utf8(p.birthday, sizeof(p.birthday), a->fields[2].buf);
        aic_strlcpy_utf8(p.occupation, sizeof(p.occupation), a->fields[3].buf);
        aic_strlcpy_utf8(p.extra_note, sizeof(p.extra_note), a->fields[4].buf);
        aic_strlcpy_utf8(p.avatar_path, sizeof(p.avatar_path), a->fields[5].buf);
        rc = aic_profile_set(a->db, &p, err, sizeof(err));
        if (rc == AIC_OK) {
            app_status(a, "个人信息已保存");
            aic_guide_mark(a->db, "profile_fill", 0);
            app_publish(a, AIC_ENT_PROFILE, AIC_OP_UPDATE, "1");
        }
        break;
    }
    case FORM_RENAME: {
        rc = aic_conv_update_meta(a->db, a->conv_id, a->fields[0].buf, NULL, NULL);
        if (rc == AIC_OK) {
            aic_strlcpy_utf8(a->conv_title, sizeof(a->conv_title), a->fields[0].buf);
            app_status(a, "会话已重命名为「%s」", a->conv_title);
            app_publish(a, AIC_ENT_CONVERSATION, AIC_OP_UPDATE, a->conv_id);
        }
        break;
    }
    case FORM_EDITMSG: {
        aic_message before_msg;
        memset(&before_msg, 0, sizeof(before_msg));
        if (aic_msg_get(a->db, a->sel_msg, &before_msg) == AIC_OK) {
            aic_strlcpy_utf8(a->undo_msg, sizeof(a->undo_msg), a->sel_msg);
            aic_strlcpy_utf8(a->undo_prev, sizeof(a->undo_prev),
                             before_msg.content ? before_msg.content : "");
            a->undo_kind = 2;
            a->undo_until = aic_now_ms() + 120000;
            aic_msg_free(&before_msg);
        }
        if (a->editing_ai) {
            /* FR-36 / §9.2：改 AI 发言不设时间窗，同一事务里按 source_message_id 物理删除旧记忆条目
             * 再重新抽取；失败整体回滚。成功后把 memory_replace 事件发给对端。 */
            int removed = 0;
            int added = 0;
            rc = aic_engine_edit_ai(a->eng, a->sel_msg, a->fields[0].buf, &removed, &added,
                                    err, sizeof(err));
            if (rc == AIC_OK) {
                app_load_chat(a);
                app_publish(a, AIC_ENT_MEMORY, AIC_OP_UPDATE, a->sel_msg);
                app_publish(a, AIC_ENT_MESSAGE, AIC_OP_EDIT, a->sel_msg);
                app_status(a, "AI 发言已修改，记忆已重建（删旧 %d 条 / 抽新 %d 条）", removed, added);
            }
        } else {
            rc = aic_engine_edit(a->eng, a->sel_msg, a->fields[0].buf, err, sizeof(err));
            if (rc == AIC_OK) {
                app_load_chat(a);
                app_status(a, "消息已修改（首次修改会保留原文）");
                app_publish(a, AIC_ENT_MESSAGE, AIC_OP_EDIT, a->sel_msg);
            }
        }
        break;
    }
    default:
        app_close_overlay(a);
        return;
    }

    if (rc != AIC_OK) {
        app_status(a, "保存失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    app_close_overlay(a);
    app_reload_threads(a);
    app_invalidate(a);
}

/* ---------- 知识库 / 错误中心 / 设备权限的动作（FR-31/32/34/35） ---------- */
/* 列表页的行先排一遍、再照同一顺序画，点击落在 ACT_KB_ITEM_BASE + 行号上。 */
static char g_kb_row_id[96][64];        /* 条目 id；分组标题行留空 */
static char g_kb_row_label[96][160];    /* 行文字 */
static int  g_kb_row_bm[96];            /* 是否已加书签 */
static int  g_kb_row_kind[96];          /* 0 = 分组标题，1 = 条目 */
static int  g_kb_row_n;

static void kb_search_run(App *a)
{
    if (!a->kb) { app_status(a, "知识包未载入"); return; }
    a->kb_hit_n = 0;
    memset(a->kb_hits, 0, sizeof(a->kb_hits));
    if (!a->kb_query[0]) { app_status(a, "先输入检索关键词"); return; }
    a->kb_hit_n = aic_kb_search(a->kb, a->kb_query, &a->kb_hits[0][0],
                                (int)sizeof(a->kb_hits[0]),
                                (int)(sizeof(a->kb_hits) / sizeof(a->kb_hits[0])), NULL);
    if (a->kb_hit_n <= 0) {
        a->kb_hit_n = 0;
        app_status(a, "没有命中「%s」：换个词，或按章节翻", a->kb_query);
    } else {
        app_status(a, "命中 %d 篇（只看标题、摘要、要点与正文，不外链）", a->kb_hit_n);
    }
    a->kb_focus = 0;
    a->scroll[1] = 0;
}

static void cap_leave_subpage(App *a)
{
    a->page_kb = 0;
    a->page_error = 0;
    a->page_cap = 0;
    a->page_advanced = 0;
    a->page_pair = 0;
}

/* 返回 1 表示这次动作已经处理完。 */
static int app_action_tdd(App *a, int act)
{
    if (act == ACT_ROW_KB) {
        cap_leave_subpage(a);
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_kb = 1;
        a->scroll[1] = 0;
        a->kb_hit_n = 0;
        a->kb_article[0] = '\0';
        a->kb_para = -1;
        app_kb_refresh(a);
        return 1;
    }
    if (act == ACT_ROW_ERROR) {
        cap_leave_subpage(a);
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_error = 1;
        a->scroll[1] = 0;
        app_err_reload(a);
        aic_error_ack_all(a->db);   /* 看过就不再算「未查看」，条目仍留在列表里 */
        return 1;
    }
    if (act == ACT_ROW_CAP) {
        cap_leave_subpage(a);
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_cap = 1;
        a->scroll[1] = 0;
        a->app_n = 0;
        a->cap_msg[0] = '\0';
        return 1;
    }

    /* 内置知识库 */
    if (act >= ACT_KB_ITEM_BASE && act < ACT_KB_ITEM_BASE + 96) {
        int i = act - ACT_KB_ITEM_BASE;
        if (i >= 0 && i < g_kb_row_n && g_kb_row_id[i][0]) {
            app_kb_open(a, g_kb_row_id[i]);
            a->kb_hit_n = 0;
        }
        return 1;
    }
    if (act >= ACT_KB_PARA_BASE && act < ACT_KB_PARA_BASE + 16) {
        a->kb_para = act - ACT_KB_PARA_BASE;
        return 1;
    }
    if (act == ACT_KB_BACK) {
        if (a->kb_article[0]) {
            a->kb_article[0] = '\0';
            a->kb_para = -1;
        } else {
            a->page_kb = 0;
        }
        a->scroll[1] = 0;
        return 1;
    }
    if (act == ACT_KB_SEARCH_EDIT) {
        a->kb_focus = 1;
        a->cred_focus = 0;
        a->pair_focus = 0;
        return 1;
    }
    if (act == ACT_KB_SEARCH_GO) { kb_search_run(a); return 1; }
    if (act == ACT_KB_BOOKMARK) {
        int on = 0;
        if (a->kb_article[0] && aic_kb_bookmark_toggle(a->db, a->kb_article, &on) == AIC_OK) {
            app_publish_setting(a, AIC_SET_KB_BOOKMARK);
            app_status(a, on ? "已加入书签" : "已取消书签");
        }
        return 1;
    }
    if (act == ACT_KB_CITE) {
        sb cite;
        int rc;
        if (!a->kb || !a->kb_article[0]) return 1;
        sb_init(&cite);
        rc = aic_kb_cite(a->kb, a->kb_article, a->kb_para < 0 ? 0 : a->kb_para, &cite);
        if (rc == AIC_OK) {
            const aic_kb_article *ar = aic_kb_find(a->kb, a->kb_article);
            aic_setting_set(a->db, AIC_SET_KB_CITE, sb_cstr(&cite));
            app_rebuild_fragment(a);
            app_status(a, "已插入引用片段（来自《%s》，只带这一段与来源，不携全文）",
                       ar ? ar->title : "知识库");
        } else {
            app_status(a, "引用失败：%s", aic_strerror(rc));
        }
        sb_free(&cite);
        return 1;
    }
    if (act == ACT_KB_CITE_CLEAR) {
        aic_setting_del(a->db, AIC_SET_KB_CITE);
        app_rebuild_fragment(a);
        app_status(a, "已清除知识库引用片段，注入区恢复成只含插件扩展点");
        return 1;
    }

    /* 错误中心 */
    if (act == ACT_ERR_BACK) { a->page_error = 0; a->scroll[1] = 0; return 1; }
    if (act == ACT_ERR_ACK) {
        aic_error_ack_all(a->db);
        app_err_reload(a);
        app_status(a, "已全部标为已查看");
        return 1;
    }
    if (act == ACT_ERR_CLEAR) {
        int removed = 0;
        aic_error_clear(a->db, 0, &removed);
        app_err_reload(a);
        app_status(a, "已清除 %lld 条错误记录（明细文件一并删除）", (long long)removed);
        return 1;
    }
    if (act == ACT_ERR_EXPORT) {
        char dir[800];
        char path[900];
        char err[256];
        int rc;
        snprintf(dir, sizeof(dir), "%s\\exports", a->root);
        aic_mkdir_p(dir);
        snprintf(path, sizeof(path), "%s\\exports\\diagnostic-%lld.json",
                 a->root, (long long)aic_now_ms());
        err[0] = '\0';
        rc = aic_error_export_diagnostic(a->db, path, err, sizeof(err));
        if (rc == AIC_OK)
            app_status(a, "诊断包已导出：%s（原始响应已脱敏，不含 API Key）", path);
        else
            app_status(a, "导出诊断包失败：%s", err[0] ? err : aic_strerror(rc));
        return 1;
    }
    if (act >= ACT_ERR_CAT_BASE && act <= ACT_ERR_CAT_BASE + g_err_cat_n) {
        a->err_filter = act - ACT_ERR_CAT_BASE;
        a->scroll[1] = 0;
        app_err_reload(a);
        return 1;
    }
    if (act >= ACT_ERR_ACT_BASE && act < ACT_ERR_ACT_BASE + 200) {
        int i = act - ACT_ERR_ACT_BASE;
        if (i >= 0 && i < a->err_n) {
            const aic_error_entry *er = &a->err_rows[i];
            int what = aic_error_action_for(er->category);
            if (what == AIC_ERR_ACT_RETRY) {
                a->page_error = 0;
                aic_router_set_tab(a->router, AIC_TAB_CHAT);
                app_status(a, "已回到聊天：原草稿还在输入框，点发送即重试，不产生新的用户消息");
            } else if (what == AIC_ERR_ACT_BALANCE) {
                a->page_error = 0;
                app_status(a, "在设置页「余额与用量」查看上次结果与过期时间");
            } else if (what == AIC_ERR_ACT_SETTINGS) {
                a->page_error = 0;
                app_status(a, "在设置页「API Key」检查凭证；凭证失效不清会话，改完接着发");
            } else if (what == AIC_ERR_ACT_CAPABILITY) {
                cap_leave_subpage(a);
                a->page_cap = 1;
                a->scroll[1] = 0;
            } else if (what == AIC_ERR_ACT_DETAIL) {
                if (er->detail_path[0])
                    snprintf(a->overlay_text, sizeof(a->overlay_text),
                             "脱敏后的原始响应在数据目录的 %s。诊断包导出会把它一起带走，"
                             "但不会包含 API Key 或完整机器信息。", er->detail_path);
                else
                    aic_strlcpy_utf8(a->overlay_text, sizeof(a->overlay_text),
                                     "这条错误没有保留原始响应。");
                a->confirm_action = 0;
                app_set_overlay(a, AIC_OV_CONFIRM, "错误明细");
            }
            a->scroll[1] = 0;
        }
        return 1;
    }

    /* 设备权限 */
    if (act == ACT_CAP_BACK) {
        a->page_cap = 0;
        a->app_n = 0;
        a->cap_msg[0] = '\0';
        a->scroll[1] = 0;
        return 1;
    }
    if (act >= ACT_CAP_TOGGLE_BASE && act < ACT_CAP_TOGGLE_BASE + AIC_CAP_COUNT) {
        int i = act - ACT_CAP_TOGGLE_BASE;
        const char *kind = aic_cap_kind_at(i);
        aic_cap_status st;
        if (!kind) return 1;
        memset(&st, 0, sizeof(st));
        aic_cap_status_load(a->db, kind, &st);
        if (st.granted) {
            int rc = aic_cap_revoke(a->db, kind);
            if (rc == AIC_OK) app_status(a, "已撤销「%s」授权（对话与同步保持可用）", aic_cap_name(kind));
            else app_status(a, "撤销失败：%s", aic_strerror(rc));
        } else {
            char purpose[512];
            int rc;
            purpose[0] = '\0';
            rc = aic_cap_request(a->db, kind, purpose, sizeof(purpose));
            if (rc != AIC_OK) { app_status(a, "申请失败：%s", aic_strerror(rc)); return 1; }
            /* 申请前先把用途说清楚，再授予（§18）。 */
            snprintf(a->cap_msg, sizeof(a->cap_msg), "「%s」用途：%s", aic_cap_name(kind),
                     purpose[0] ? purpose : aic_cap_purpose(kind));
            rc = aic_cap_grant(a->db, kind, aic_now_ms());
            if (rc == AIC_OK) app_status(a, "已授予「%s」：%s", aic_cap_name(kind), a->cap_msg);
            else app_status(a, "授予失败：%s", aic_strerror(rc));
        }
        return 1;
    }
    if (act == ACT_CAP_PICK_MEDIA) {
        if (cap_pick_path(a, L"图片 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0\0",
                          AIC_SET_CAP_PREFIX "media.selected", L"选一张图片（只记名称与大小）"))
            app_status(a, "已选中图片：注入时只给名称与大小，原图不上传");
        return 1;
    }
    if (act == ACT_CAP_PICK_FILES) {
        if (cap_pick_path(a, L"文本文件 (*.txt;*.md;*.json;*.log)\0*.txt;*.md;*.json;*.log\0所有文件\0*.*\0\0",
                          AIC_SET_CAP_PREFIX "files.selected", L"选一个文本文件（只读、不递归）"))
            app_status(a, "已选中文件：只在读取时注入文本片段，不传原始文件");
        return 1;
    }
    if (act == ACT_CAP_PICK_APPS) {
        int n = app_enum_apps(a);
        if (n <= 0) aic_strlcpy_utf8(a->cap_msg, sizeof(a->cap_msg),
                                     "没有读到已安装软件列表：可以先允许读取，或直接跳过这一项。");
        else snprintf(a->cap_msg, sizeof(a->cap_msg),
                      "读到 %d 条已安装软件，从下面选一条（只取你选中的这一条，完整清单不上传）", n);
        return 1;
    }
    if (act >= ACT_CAP_APP_BASE && act < ACT_CAP_APP_BASE + 40) {
        int i = act - ACT_CAP_APP_BASE;
        if (i >= 0 && i < a->app_n) {
            aic_setting_set(a->db, AIC_SET_CAP_PREFIX "apps.selected", a->app_list[i]);
            app_publish_setting(a, AIC_SET_CAP_PREFIX "apps.selected");
            snprintf(a->cap_msg, sizeof(a->cap_msg), "已选中：%s（注入时只给这一个名字）", a->app_list[i]);
            app_status(a, "已选中「%s」", a->app_list[i]);
        }
        return 1;
    }
    return 0;
}

static void do_action(App *a, int act)
{
    aic_motion mo = aic_motion_default();

    if (app_action_tdd(a, act)) { app_invalidate(a); return; }

    /* 首次启动顺序（FR-30）与双端配对（第 16 章）。 */
    if (act == ACT_CRED_KEY_EDIT) { a->cred_focus = 1; app_invalidate(a); return; }
    if (act == ACT_CRED_SAVE)     { app_cred_save(a); return; }
    if (act == ACT_CRED_DEMO)     { app_cred_demo(a); return; }
    if (act == ACT_MODE_CHAT)     { app_mode_select(a, AIC_MODE_CHAT); return; }
    if (act == ACT_MODE_QA)       { app_mode_select(a, AIC_MODE_QA); return; }
    if (act == ACT_ROW_SYNC) {
        int on = aic_sync_realtime_enabled(a->db) ? 0 : 1;
        aic_sync_set_realtime(a->db, on);
        app_status(a, on ? "实时同步已开启（配对成功后默认开启）"
                         : "实时同步已暂停：只停新事件发送，已入队事件会保留");
        app_invalidate(a);
        return;
    }
    if (act == ACT_ROW_PAIR) { a->page_pair = 1; a->scroll[1] = 0; app_sync_start(a); app_invalidate(a); return; }
    if (act == ACT_PAIR_BACK) { a->page_pair = 0; a->scroll[1] = 0; app_invalidate(a); return; }
    if (act == ACT_PAIR_CODE_EDIT) { a->pair_focus = 1; a->cred_focus = 0; app_invalidate(a); return; }
    if (act == ACT_PAIR_ADDR_EDIT) { a->pair_focus = 2; a->cred_focus = 0; app_invalidate(a); return; }
    if (act == ACT_PAIR_HOST)      { app_pair_start(a, 1); return; }
    if (act == ACT_PAIR_DISCOVER)  { app_pair_start(a, 2); return; }
    if (act == ACT_PAIR_CONNECT)   { app_pair_start(a, 3); return; }
    if (act == ACT_PAIR_HANDSHAKE) { app_pair_start(a, 4); return; }
    if (act == ACT_PAIR_TOGGLE) {
        int on = aic_sync_realtime_enabled(a->db) ? 0 : 1;
        aic_sync_set_realtime(a->db, on);
        app_status(a, on ? "已恢复实时同步" : "已暂停实时同步，事件留在本地队列");
        app_invalidate(a);
        return;
    }
    if (act == ACT_PAIR_CONFLICTS) {
        sb cs;
        sb_init(&cs);
        if (aic_sync_conflicts_json(a->db, &cs) != AIC_OK || !sb_cstr(&cs)[0])
            aic_strlcpy_utf8(a->overlay_text, sizeof(a->overlay_text),
                             "目前没有冲突副本。并发修改按逻辑时钟取较大者，落败方原始实体留副本。");
        else
            aic_strlcpy_utf8(a->overlay_text, sizeof(a->overlay_text), sb_cstr(&cs));
        sb_free(&cs);
        app_set_overlay(a, AIC_OV_CONFIRM, "冲突副本");
        a->confirm_action = 0;
        app_invalidate(a);
        return;
    }
    if (act == ACT_PAIR_UNPAIR) { app_pair_unpair(a); return; }
    if (act == ACT_PAIR_CH_LAN)   { app_pair_set_channel(a, AIC_CH_LAN); return; }
    if (act == ACT_PAIR_CH_BT)    { app_pair_set_channel(a, AIC_CH_BLUETOOTH); return; }
    if (act >= ACT_PAIR_DISCOVER + 100 && act < ACT_PAIR_DISCOVER + 108) {
        int i = act - (ACT_PAIR_DISCOVER + 100);
        if (i >= 0 && i < a->peer_n) {
            aic_strlcpy_utf8(a->pair_addr_in, sizeof(a->pair_addr_in), a->peers[i].addr);
            a->pair_focus = 2;
            if (a->peers[i].channel == AIC_CH_BLUETOOTH) {
                app_status(a, "已填入蓝牙设备 %s（%s），再点「连到该地址并配对」",
                           a->peers[i].name, a->peers[i].addr);
            } else {
                app_status(a, "已填入 %s:%d，再点「连到该地址并配对」", a->peers[i].addr, a->peers[i].port);
            }
        }
        app_invalidate(a);
        return;
    }

    if (act >= ACT_GUIDE_ITEM_BASE && act < ACT_GUIDE_ITEM_BASE + AIC_GUIDE_STEP_COUNT) {
        const aic_guide_step *steps = aic_guide_steps();
        int i = act - ACT_GUIDE_ITEM_BASE;
        aic_guide_mark(a->db, steps[i].id, 0);
        app_status(a, "已记录完成：%s", steps[i].title);
        app_invalidate(a);
        return;
    }
    if (act >= ACT_QA_THREAD_BASE && act < ACT_QA_THREAD_BASE + 200) {
        int i = act - ACT_QA_THREAD_BASE;
        if ((size_t)i < a->thread_n) {
            aic_router_set_thread(a->router, a->threads[i].id);
            app_load_chat(a);
            a->scroll[2] = 0;
            app_invalidate(a);
        }
        return;
    }
    if (act >= ACT_MSG_BASE && act < ACT_MSG_BASE + MSG_CAP) {
        app_msg_open(a, act - ACT_MSG_BASE);
        return;
    }
    if (act >= ACT_PLUGIN_TOGGLE_BASE && act < ACT_PLUGIN_TOGGLE_BASE + 100) {
        app_plugin_toggle(a, (size_t)(act - ACT_PLUGIN_TOGGLE_BASE));
        app_invalidate(a);
        return;
    }
    if (act >= ACT_PLUGIN_UPDATE_BASE && act < ACT_PLUGIN_UPDATE_BASE + 100) {
        app_plugin_update(a, (size_t)(act - ACT_PLUGIN_UPDATE_BASE));
        app_invalidate(a);
        return;
    }
    if (act >= ACT_COPY_BASE && act < ACT_COPY_BASE + MSG_CAP * 8) {
        app_copy_block(a, act);
        app_invalidate(a);
        return;
    }
    if (act >= ACT_PERSONA_ROLLBACK_BASE && act < ACT_PERSONA_ROLLBACK_BASE + 8) {
        app_persona_rollback(a, act - ACT_PERSONA_ROLLBACK_BASE);
        app_invalidate(a);
        return;
    }
    if (act >= ACT_FIELD_BASE) {
        int i = act - ACT_FIELD_BASE;
        if (i >= 0 && i < a->field_n) {
            a->field_focus = i;
            app_invalidate(a);
        }
        return;
    }

    switch (act) {
    case ACT_STOP:
        if (a->eng && a->sending) {
            aic_engine_stop(a->eng);
            app_status(a, "已请求停止生成");
        }
        break;
    case ACT_REGENERATE:
        app_regenerate(a);
        break;
    case ACT_NAV_ME:
        aic_router_set_tab(a->router, AIC_TAB_ME);
        app_begin_anim(a, 2, mo.tab_switch_ms);
        break;
    case ACT_NAV_SETTINGS:
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        app_begin_anim(a, 2, mo.tab_switch_ms);
        break;
    case ACT_NAV_CHAT:
        if (aic_router_mode(a->router) == AIC_MODE_QA) {
            aic_router_set_thread(a->router, "");
            app_load_chat(a);
            aic_router_set_tab(a->router, AIC_TAB_CHAT);
        } else {
            aic_router_set_tab(a->router, AIC_TAB_CHAT);
        }
        app_begin_anim(a, 2, mo.tab_switch_ms);
        break;
    case ACT_MODE_SWITCH: {
        aic_mode target = aic_router_mode(a->router) == AIC_MODE_CHAT ? AIC_MODE_QA : AIC_MODE_CHAT;
        int rc = aic_router_switch_mode(a->router, target);
        if (rc != AIC_OK) {
            app_status(a, "模式切换被忽略（%s）", aic_strerror(rc));
            break;
        }
        aic_router_finish_switch(a->router);
        if (target == AIC_MODE_CHAT) aic_router_set_tab(a->router, AIC_TAB_CHAT);
        app_load_chat(a);
        app_reload_threads(a);
        a->scroll[2] = 0;
        app_begin_anim(a, 1, mo.mode_switch_ms);
        app_status(a, "已切换到%s", target == AIC_MODE_CHAT ? "聊天模式" : "问答模式");
        break;
    }
    case ACT_SEND:
        app_send(a);
        break;
    case ACT_AI_INFO:
        if (aic_router_mode(a->router) == AIC_MODE_CHAT) {
            app_persona_edit(a);
        } else {
            app_persona_edit(a);
        }
        break;
    case ACT_PROFILE_EDIT:  app_profile_edit(a); break;
    case ACT_GUIDE_VIEW:    app_guide(a); break;
    case ACT_QA_NEW:        app_new_thread(a); break;
    case ACT_QA_SEARCH:
        a->search_focus = 1;
        app_status(a, "输入关键词搜索会话标题（Esc 退出搜索）");
        break;
    case ACT_QA_BACK:
        aic_router_set_thread(a->router, "");
        app_load_chat(a);
        app_invalidate(a);
        break;
    case ACT_QA_DELETE: {
        char err[256] = {0};
        int rc = aic_conv_delete(a->db, a->conv_id);
        if (rc == AIC_OK) {
            aic_router_set_thread(a->router, "");
            app_reload_threads(a);
            app_load_chat(a);
            app_status(a, "会话已删除");
        } else {
            app_status(a, "删除失败：%s", err[0] ? err : aic_strerror(rc));
        }
        break;
    }
    case ACT_ROW_THEME: {
        char cur[16] = {0};
        const char *next = "0";
        aic_setting_get(a->db, AIC_SET_THEME, cur, sizeof(cur));
        if (cur[0] == '0' || !cur[0]) next = "1";
        else if (cur[0] == '1') next = "2";
        else next = "0";
        aic_setting_set(a->db, AIC_SET_THEME, next);
        app_publish_setting(a, AIC_SET_THEME);
        app_status(a, "配色切换为：%s",
                   next[0] == '0' ? "日光" : (next[0] == '1' ? "夜间" : "跟随系统"));
        break;
    }
    case ACT_ROW_MODEL: {
        char cur[64] = {0};
        aic_setting_get(a->db, AIC_SET_MODEL_DEFAULT, cur, sizeof(cur));
        if (aic_str_ieq(cur, AIC_MODEL_PRO)) {
            aic_setting_set(a->db, AIC_SET_MODEL_DEFAULT, AIC_MODEL_FLASH);
            app_status(a, "默认模型：%s", aic_llm_model_find(AIC_MODEL_FLASH)->name);
        } else {
            aic_setting_set(a->db, AIC_SET_MODEL_DEFAULT, AIC_MODEL_PRO);
            app_status(a, "默认模型：%s", aic_llm_model_find(AIC_MODEL_PRO)->name);
        }
        app_setup_engine(a);
        break;
    }
    case ACT_ROW_KEY:      app_key_edit(a); break;
    case ACT_ROW_EXPORT:
        app_set_overlay(a, AIC_OV_CONFIRM, "导出聊天记录");
        a->confirm_action = FORM_EXPORT;
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "导出到 %s\\exports。JSON 可再次导入（含 format/schema_version/checksum）；"
                 "Markdown 与纯文本仅供阅读，不参与导入。导出包不含凭证。", a->root);
        break;
    case ACT_ROW_IMPORT:   app_import(a); break;
    case ACT_ROW_PLUGIN:   app_plugin_panel(a); break;
    case ACT_ROW_BALANCE:  app_balance(a); app_balance_refresh(a); break;
    case ACT_ROW_UPDATE:   app_check_update(a, 1); break;
    case ACT_ROW_GUIDE:    app_guide(a); break;
    case ACT_ROW_DATA:     app_data(a); break;
    case ACT_ROW_ABOUT:
        app_set_overlay(a, AIC_OV_CONFIRM, "关于 Sai");
        snprintf(a->overlay_text, sizeof(a->overlay_text),
                 "Sai %s\n核心逻辑用 C 实现（C11），界面层调用平台原生能力；"
                 "会话数据格式 Windows 与 Android 完全一致。\n\n"
                 "模型请求走平台自有网络通道（Windows 用 WinHTTP，Android 用 HttpURLConnection），"
                 "API Key 存放在系统安全存储里，不经第三方中转；没有配置 Key 时可用演示模式预览界面。"
                 "更新安装、插件隔离与备份恢复见设置页与操作指南。", APP_VERSION);
        break;
    case ACT_ROW_MOTION: {
        int on = aic_setting_get_int(a->db, AIC_SET_MOTION, 1);
        aic_setting_set_int(a->db, AIC_SET_MOTION, on ? 0 : 1);
        app_status(a, "减少动态效果：%s", on ? "开启（动画瞬时完成）" : "关闭");
        break;
    }
    case ACT_EXPORT_JSON:  app_export(a, 0); break;
    case ACT_EXPORT_MD:    app_export(a, 1); break;
    case ACT_EXPORT_TXT:   app_export(a, 2); break;
    case ACT_AVATAR_PICK:
        app_avatar_pick(a);
        break;
    case ACT_BACKUP_NOW:
        app_backup_now(a);
        break;
    case ACT_BACKUP_RESTORE:
        app_backup_restore(a);
        break;
    case ACT_BACKUP_CLEAN:
        app_backup_clean(a);
        break;
    case ACT_UPDATE_NOW:
        app_update_now(a);
        break;
    case ACT_UPDATE_LATER:
        app_close_overlay(a);
        app_status(a, "稍后提醒：启动时与固定周期会再次检查");
        break;
    case ACT_PLUGIN_IMPORT_ACT:
        app_plugin_import(a);
        app_plugin_panel(a);
        break;
    case ACT_PLUGIN_PANEL_CALL:
        app_plugin_panel_call(a);
        break;
    case ACT_UPDATE_TEST:  app_update_test(a); break;
    case ACT_UPDATE_SKIP:
        a->confirm_action = 0;
        app_close_overlay(a);
        app_status(a, "已跳过该版本（下次检查仍会提示强制更新）");
        break;
    case ACT_MODEL_FLASH:
    case ACT_MODEL_PRO:
        aic_setting_set(a->db, AIC_SET_MODEL_DEFAULT,
                        act == ACT_MODEL_PRO ? AIC_MODEL_PRO : AIC_MODEL_FLASH);
        app_publish_setting(a, AIC_SET_MODEL_DEFAULT);
        app_setup_engine(a);
        app_close_overlay(a);
        break;
    case ACT_PLUGIN_INSTALL_CONFIRM:
        app_plugin_import_confirm(a);
        app_close_overlay(a);
        app_invalidate(a);
        break;
    case ACT_LOAD_OLDER:
        app_load_older(a);
        app_invalidate(a);
        break;
    case ACT_MSG_COPY:
        app_copy_message(a);
        break;
    case ACT_UNDO:
        app_undo(a);
        break;
    case ACT_QA_RENAME:
        app_qa_rename(a);
        break;
    case ACT_ROW_ADVANCED:
        a->page_advanced = a->page_advanced ? 0 : 1;
        a->scroll[1] = 0;
        break;
    case ACT_AI_AVATAR:
        app_ai_avatar_pick(a);
        break;
    case ACT_AI_VERSIONS:
        app_ai_versions(a);
        break;
    case ACT_AI_MODEL_FLASH:
    case ACT_AI_MODEL_PRO: {
        const char *mid = (act == ACT_AI_MODEL_PRO) ? AIC_MODEL_PRO : AIC_MODEL_FLASH;
        if (a->conv_id[0]) aic_conv_update_meta(a->db, a->conv_id, NULL, mid, NULL);
        if (a->eng) aic_engine_set_model_override(a->eng, mid);
        app_status(a, "本会话模型：%s（只影响后续请求）", aic_llm_model_find(mid)->name);
        break;
    }
    case ACT_SCOPE_CUR:
        a->export_scope = 0;
        app_status(a, "导出范围：当前会话");
        break;
    case ACT_SCOPE_ALL:
        a->export_scope = 1;
        app_status(a, "导出范围：当前模式下的全部会话");
        break;
    case ACT_SCOPE_RANGE:
        a->export_scope = 2;
        app_status(a, "导出范围：近 7 天");
        break;
    case ACT_SCOPE_AVATAR:
        a->export_avatars = a->export_avatars ? 0 : 1;
        app_status(a, a->export_avatars ? "导出将包含头像图片" : "导出不含头像");
        break;
    case ACT_OVERLAY_CLOSE:
    case ACT_FIELD_CANCEL:
        app_try_close(a);
        break;
    case ACT_FIELD_SAVE:
        app_save_form(a);
        break;
    case ACT_CONFIRM_YES:
        if (a->quit_confirm) {
            DestroyWindow(a->hwnd);
            return;
        }
        app_close_overlay(a);
        break;
    case ACT_CONFIRM_NO:
        app_close_overlay(a);
        break;
    case ACT_GUIDE_RESET:
        aic_guide_reset(a->db);
        app_status(a, "新手指南进度已重置");
        break;
    case ACT_MSG_RECALL: {
        char err[256] = {0};
        int window = aic_setting_get_int(a->db, AIC_SET_RECALL_WINDOW, AIC_RECALL_WINDOW_DEFAULT);
        int rc = aic_engine_recall(a->eng, a->sel_msg, window, err, sizeof(err));
        if (rc == AIC_OK) {
            aic_strlcpy_utf8(a->undo_msg, sizeof(a->undo_msg), a->sel_msg);
            a->undo_kind = 1;
            a->undo_until = aic_now_ms() + 120000;
            app_load_chat(a);
            app_close_overlay(a);
            app_status(a, "消息已撤回（正文替换为系统提示，不再进入下一次请求）");
            app_toast(a, "已撤回，2 分钟内可在这里恢复");
        } else {
            app_status(a, "撤回失败：%s", err[0] ? err : aic_strerror(rc));
        }
        break;
    }
    case ACT_MSG_EDIT: {
        aic_message m;
        memset(&m, 0, sizeof(m));
        if (aic_msg_get(a->db, a->sel_msg, &m) != AIC_OK) break;
        a->editing_ai = (strcmp(m.role, aic_role_name(AIC_ROLE_ASSISTANT)) == 0);
        if (a->editing_ai) {
            /* §9.2：改 AI 发言前必须说清它同时改记忆，避免用户以为只改显示。 */
            app_set_overlay(a, AIC_OV_CONFIRM, "修改 AI 发言（会同步更改 AI 记忆）");
            a->confirm_action = FORM_EDITMSG;
            fld(a, "新的内容（修改将同步更改 AI 记忆：旧记忆条目会被删除并重新抽取）",
                m.content ? m.content : "", 1, 0);
        } else {
            app_set_overlay(a, AIC_OV_CONFIRM, "修改消息");
            a->confirm_action = FORM_EDITMSG;
            fld(a, "新的内容（原内容会保留）", m.content ? m.content : "", 1, 0);
        }
        a->field_focus = 0;
        aic_msg_free(&m);
        break;
    }
    default:
        break;
    }
    app_invalidate(a);
}


/* ---------- 复制 / 撤销 / 重命名 / 版本回退 / 进阶指南 ---------- */
/* 技术设计 6.1（代码块复制）、9.2（短时撤销）、6.3（会话重命名）、7.2（版本回退）、10.3 与 12.3（进阶指南）。 */
static void app_copy_text(App *a, const char *text)
{
    size_t cap;
    wchar_t *w;
    size_t n;
    HGLOBAL h;
    void *dst;

    if (!text || !text[0]) { app_status(a, "没有可复制的内容"); return; }
    if (!OpenClipboard(a->hwnd)) { app_status(a, "打开剪贴板失败，请稍后重试"); return; }
    EmptyClipboard();
    cap = strlen(text) + 2;
    w = (wchar_t *)aic_xmalloc(cap * sizeof(wchar_t));
    u8_to_w(text, w, (int)cap);
    n = wcslen(w);
    h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
    if (!h) {
        aic_xfree(w);
        CloseClipboard();
        app_status(a, "剪贴板内存不足");
        return;
    }
    dst = GlobalLock(h);
    if (!dst) {
        GlobalFree(h);
        aic_xfree(w);
        CloseClipboard();
        app_status(a, "剪贴板内存不足");
        return;
    }
    memcpy(dst, w, (n + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    aic_xfree(w);
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);
        CloseClipboard();
        app_status(a, "写入剪贴板失败");
        return;
    }
    CloseClipboard();
    app_status(a, "已复制到剪贴板");
}

static void app_copy_message(App *a)
{
    aic_message m;

    memset(&m, 0, sizeof(m));
    if (aic_msg_get(a->db, a->sel_msg, &m) != AIC_OK) {
        app_status(a, "消息已不存在");
        return;
    }
    app_copy_text(a, m.recalled ? AIC_RECALL_NOTICE : (m.content ? m.content : ""));
    aic_msg_free(&m);
    app_close_overlay(a);
}

static void app_copy_block(App *a, int act)
{
    int idx = (act - ACT_COPY_BASE) / 8;
    int blk = (act - ACT_COPY_BASE) % 8;
    int n;

    if (idx < 0 || (size_t)idx >= a->msg_n) return;
    n = md_split(a->msgs[idx].content ? a->msgs[idx].content : "", g_md, MD_BLK_MAX);
    if (blk < 0 || blk >= n) return;
    app_copy_text(a, g_md[blk].buf);
}

static void app_undo(App *a)
{
    int rc;

    if (!a->undo_msg[0] || aic_now_ms() > a->undo_until) {
        app_status(a, "撤销窗口已过（默认 2 分钟）");
        return;
    }
    if (a->undo_kind == 1) rc = aic_msg_unrecall(a->db, a->undo_msg);
    else if (a->undo_kind == 2) rc = aic_msg_restore(a->db, a->undo_msg);
    else rc = AIC_E_STATE;
    if (rc == AIC_OK) {
        int undo_kind = a->undo_kind;   /* 下面会清零，先留住语义 */
        app_load_chat(a);
        app_status(a, undo_kind == 1 ? "已恢复撤回的消息" : "已恢复修改前的内容");
        app_publish(a, AIC_ENT_MESSAGE, undo_kind == 1 ? AIC_OP_UPDATE : AIC_OP_EDIT,
                    a->undo_msg);
        a->undo_msg[0] = 0;
        a->undo_kind = 0;
        a->undo_until = 0;
    } else {
        app_status(a, "撤销失败：%s", aic_strerror(rc));
    }
}

static void app_qa_rename(App *a)
{
    if (!a->conv_id[0]) { app_status(a, "当前没有打开的会话"); return; }
    app_set_overlay(a, AIC_OV_CONFIRM, "重命名会话");
    a->confirm_action = FORM_RENAME;
    fld(a, "会话标题（最长 200 个字符）", a->conv_title, 0, 0);
    a->field_focus = 0;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "改名只影响列表显示，不改变消息内容与更新时间。");
}

static void app_ai_versions(App *a)
{
    aic_persona per;
    aic_prompt_version *vs = NULL;
    size_t n = 0;
    int i;

    memset(&per, 0, sizeof(per));
    aic_persona_ensure_default(a->db, &per);
    aic_persona_versions(a->db, per.id, &vs, &n);
    aic_strlcpy_utf8(a->ver_persona, sizeof(a->ver_persona), per.id);
    a->ver_n = 0;
    for (i = 0; i < (int)n && i < 8; i++) {
        a->ver_num[i] = vs[i].version;
        aic_strlcpy_utf8(a->ver_time[i], sizeof(a->ver_time[i]), vs[i].created_at);
        a->ver_n = i + 1;
    }
    if (vs) aic_prompt_version_free_array(vs, n);
    app_set_overlay(a, AIC_OV_CONFIRM, "提示词版本");
    a->confirm_action = FORM_VERSIONS;
    snprintf(a->overlay_text, sizeof(a->overlay_text),
             "当前版本 v%d。选一个旧版本回退：回退会用旧版内容生成新版本，历史版本全部保留可回溯。",
             per.current_version);
    aic_persona_free(&per);
}

static void app_persona_rollback(App *a, int idx)
{
    aic_persona per;
    aic_prompt_version *vs = NULL;
    size_t n = 0;
    size_t i;
    int want;
    int done = 0;

    if (idx < 0 || idx >= a->ver_n) return;
    want = a->ver_num[idx];
    memset(&per, 0, sizeof(per));
    aic_persona_ensure_default(a->db, &per);
    aic_persona_versions(a->db, per.id, &vs, &n);
    for (i = 0; i < n; i++) {
        if (vs[i].version == want) {
            aic_persona out;
            memset(&out, 0, sizeof(out));
            if (aic_persona_update(a->db, per.id, per.name, per.description, vs[i].content, &out) == AIC_OK) {
                app_status(a, "已回退到 v%d，并生成新版本 v%d", want, out.current_version);
                aic_persona_free(&out);
                done = 1;
            }
            break;
        }
    }
    if (!done) app_status(a, "回退失败：找不到该版本的内容");
    if (vs) aic_prompt_version_free_array(vs, n);
    aic_persona_free(&per);
    app_ai_versions(a);
}

static void app_ai_avatar_pick(App *a)
{
    wchar_t wf[900];
    char src[900];
    char rel[600];
    char err[512] = {0};
    int rc;

    if (!pick_file(a->hwnd, L"图片文件 (*.jpg;*.png;*.bmp)\0*.jpg;*.jpeg;*.png;*.bmp;*.gif\0所有文件 (*.*)\0*.*\0",
                  wf, 900))
        return;
    w_to_u8(wf, src, sizeof(src));
    rc = aic_avatar_import(src, a->root, rel, sizeof(rel), err, sizeof(err));
    if (rc != AIC_OK) {
        app_status(a, "AI 头像处理失败：%s", err[0] ? err : aic_strerror(rc));
        return;
    }
    aic_setting_set(a->db, "persona.avatar", rel);
    app_status(a, "AI 头像已更新（压缩到最长边 512 像素）");
}

/* 进阶指南：插件制作、扩展点、提示词变量、主题与动效、记录格式与迁移、故障排查。 */
typedef struct AdvSec { const char *title; const char *body; } AdvSec;

static const AdvSec g_adv[] = {
    { "一、插件制作",
      "插件是一个目录：plugin.json 清单 + 入口动态库 + 可选资源。清单字段为 id / name / version /"
      " author / entry（相对路径）/ permissions / extension_points，可选 sha256 用于安装前校验入口文件哈希。"
      " 入口符号为 aic_plugin_call，签名为 int aic_plugin_call(const char *request_json, char *out,"
      " size_t out_cap)，把结果写进 out（UTF-8，NUL 结尾），返回 0 表示成功。仓库里的 plugins/sample_basic 是最小示例。" },
    { "二、扩展点",
      "prompt.fragment：读取插件目录下的 prompt_fragment.txt，按扩展点声明顺序拼进提示词。"
      " ui.panel：界面用 describe 与 invoke 两个动作调用插件面板。"
      " message.transform：改写发给模型的最后一条用户文本，聊天记录里的原文保持不变。"
      " tool.provider：向请求注入 tools 声明，具体执行由模型侧的调用协议决定。" },
    { "三、提示词与用户变量",
      "可用变量：{user_name} {user_gender} {user_birthday} {user_occupation} {user_extra_note}。"
      " 变量在请求组装阶段替换，未填写的替换为空串，不会把变量名原样发给模型。"
      " 每次修改提示词都会生成新版本，在「AI 信息」里可以回退到任意历史版本。" },
    { "四、主题与动效",
      "配色三选一：日光 / 夜间 / 跟随系统；跟随系统会读取 Windows 个性化里的应用主题，系统切换时自动跟随。"
      " 界面只从主题令牌取色，不硬编码色值；动效只改 transform 与 opacity，"
      " 开启「减少动态效果」或系统要求减少动画时，全部动画瞬时完成。" },
    { "五、记录格式与跨设备迁移",
      "导出的 JSON 包含 format=chat_record、schema_version、exported_at、source_mode、conversations、"
      " messages 与 checksum，可选包含头像图片（Base64）。导入时依次校验 format、schema_version 与 checksum，"
      " 再按冲突策略（按 id 去重 / 覆盖同名 / 保留双方）写入，任何一步失败整体回滚。"
      " Windows 与 Android 的会话数据格式完全一致，两端导出的 JSON 可以互导。"
      " 备份包与导出包互不兼容：备份包含数据库、插件数据与头像，不含凭证明文。" },
    { "六、双端配对与实时同步",
      "同一局域网直连：一端点「开始监听等待连入」生成本端 6 位配对码，另一端点「发现同网设备」"
      "（UDP 广播探测）或直接填地址连接，然后两端各自输入对方显示的码点「完成配对」。"
      " 配对码只在内存保存、有效期 5 分钟，任一端输错即终止本次配对、累计 5 次错误作废重生成；"
      "配对完成前不交换任何业务实体。配对成功后实时同步默认开启（可在设置里暂停），"
      "暂停只停新事件发送，已入队事件在恢复后按 event_id 顺序补发。"
      " 同步范围：用户资料、人设、会话、消息、提示词版本、设置白名单与插件清单；"
      "凭证、插件二进制包、用量流水原始记录、日志与备份包不参与同步。"
      " 消息顺序由逻辑时钟决定（不用墙钟），两端按逻辑时钟、来源设备与序号排序得一致序列。"
      " 同一实体两端并发修改取逻辑时钟较大者，相等比设备标识字典序，落败方原始实体留副本。"
      " 同一时刻只与一个已配对设备保持同步态；蓝牙通道只留接口未实现，当前用同一局域网直连。" },
    { "七、故障排查",
      "没有回复：先看「设置 - API Key」是否已配置，再看底部状态栏的错误提示；网络类错误会按指数退避重试。"
      " 插件不生效：在插件面板确认是否已启用；崩溃或超时的插件会被自动禁用并在设置里记录原因。"
      " 解析失败与网络异常会把原始响应写进数据目录下的 logs/，日志里的敏感串会被脱敏。" },
};

static int adv_measure(HDC dc, HFONT f, const char *txt, int width)
{
    wchar_t w[2048];
    RECT calc;
    HGDIOBJ old;

    calc.left = 0;
    calc.top = 0;
    calc.right = width;
    calc.bottom = 0;
    u8_to_w(txt, w, 2048);
    old = SelectObject(dc, f);
    DrawTextW(dc, w, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return calc.bottom - calc.top;
}

static void paint_advanced(App *a, HDC dc, RECT content)
{
    RECT y = content;
    int width;
    int needed = 56;
    int i;
    int n = (int)(sizeof(g_adv) / sizeof(g_adv[0]));

    y.left += PAD + 8;
    y.right -= PAD + 8;
    width = (int)(y.right - y.left);
    for (i = 0; i < n; i++)
        needed += adv_measure(dc, a->font_ui, g_adv[i].title, width) + 4 +
                  adv_measure(dc, a->font_small, g_adv[i].body, width) + 20;
    {
        int avail = (int)(content.bottom - content.top);
        int maxs = needed - avail;
        if (maxs < 0) maxs = 0;
        if (a->scroll[1] > maxs) a->scroll[1] = maxs;
        if (a->scroll[1] < 0) a->scroll[1] = 0;
    }
    y.top += 8 - a->scroll[1];
    draw_text(dc, a->font_title, "进阶指南",
              (RECT){y.left, y.top, y.right - 140, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_pill(a, dc, (RECT){y.right - 120, y.top + 6, y.right, y.top + 42},
              "返回设置", ACT_ROW_ADVANCED, 0);
    y.top += 52;
    for (i = 0; i < n; i++) {
        int h1 = adv_measure(dc, a->font_ui, g_adv[i].title, width);
        int h2;
        draw_text(dc, a->font_ui, g_adv[i].title,
                  (RECT){y.left, y.top, y.right, y.top + h1},
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_TOP | DT_WORDBREAK);
        y.top += h1 + 4;
        h2 = adv_measure(dc, a->font_small, g_adv[i].body, width);
        draw_text(dc, a->font_small, g_adv[i].body,
                  (RECT){y.left, y.top, y.right, y.top + h2},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
        y.top += h2 + 20;
    }
}


/* ---------- 内置知识库（FR-31 / FR-32 / §17） ---------- */
static void kb_row_add(aic_db *db, const char *id, const char *label, int kind)
{
    int slot = g_kb_row_n;
    if (slot >= (int)(sizeof(g_kb_row_id) / sizeof(g_kb_row_id[0]))) return;
    snprintf(g_kb_row_id[slot], sizeof(g_kb_row_id[0]), "%s", id ? id : "");
    aic_strlcpy_utf8(g_kb_row_label[slot], sizeof(g_kb_row_label[0]), label ? label : "");
    g_kb_row_kind[slot] = kind;
    g_kb_row_bm[slot] = (kind == 1 && db && id && id[0] && aic_kb_bookmark_has(db, id)) ? 1 : 0;
    g_kb_row_n++;
}

/* 书签与最近阅读在设置里是 id 的 JSON 数组（knowledge.c）。 */
static void kb_rows_from_ids(aic_db *db, const aic_kb *kb, const char *json, const char *section)
{
    jv *arr = NULL;
    size_t i, n;

    if (!json || !json[0]) return;
    if (jv_parse(json, strlen(json), &arr, NULL, 0) != AIC_OK || !arr) return;
    n = jv_len(arr);
    if (n > 0) kb_row_add(db, "", section, 0);
    for (i = 0; i < n; i++) {
        const char *id = jv_string(jv_at(arr, i));
        const aic_kb_article *ar = id ? aic_kb_find(kb, id) : NULL;
        if (ar) kb_row_add(db, ar->id, ar->title, 1);
    }
    jv_free(arr);
}

static void kb_build_rows(App *a)
{
    sb list;
    int gi, i;

    g_kb_row_n = 0;
    if (!a->kb) return;

    if (a->kb_hit_n > 0) {
        char sec[96];
        snprintf(sec, sizeof(sec), "检索结果（%d）", a->kb_hit_n);
        kb_row_add(a->db, "", sec, 0);
        for (i = 0; i < a->kb_hit_n; i++) {
            const aic_kb_article *ar = aic_kb_find(a->kb, a->kb_hits[i]);
            if (ar) kb_row_add(a->db, ar->id, ar->title, 1);
        }
        return;
    }

    sb_init(&list);
    if (aic_kb_bookmark_list(a->db, &list) == AIC_OK)
        kb_rows_from_ids(a->db, a->kb, sb_cstr(&list), "书签");
    sb_reset(&list);
    if (aic_kb_recent_list(a->db, &list) == AIC_OK)
        kb_rows_from_ids(a->db, a->kb, sb_cstr(&list), "最近阅读");
    sb_free(&list);

    for (gi = 0; gi < aic_kb_group_count(a->kb); gi++) {
        const aic_kb_group *g = aic_kb_group_at(a->kb, gi);
        if (!g) continue;
        kb_row_add(a->db, "", g->title, 0);
        for (i = 0; i < aic_kb_article_count(a->kb, gi); i++) {
            const aic_kb_article *ar = aic_kb_article_at(a->kb, gi, i);
            if (ar) kb_row_add(a->db, ar->id, ar->title, 1);
        }
    }
}

/* 段落与示意图节点都要按长度切出来：aic_kb_paragraph 返回的是正文里的一个区间，
 * 不以结束符收尾，直接拿去画会把后面几段一起画出来。 */
static const char *kb_para_text(const aic_kb_article *ar, int i)
{
    static char buf[AIC_KB_BODY_CAP + 1];
    int len = 0;
    const char *p = aic_kb_paragraph(ar, i, &len);
    buf[0] = '\0';
    if (!p || len <= 0) return buf;
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    memcpy(buf, p, (size_t)len);
    buf[len] = '\0';
    return buf;
}

static int kb_diagram_count(const aic_kb_article *ar)
{
    const char *p;
    int n = 1;
    if (!ar || !ar->diagram[0]) return 0;
    for (p = ar->diagram; *p; p++) if (*p == '\n') n++;
    return n;
}

static int kb_diagram_node(const aic_kb_article *ar, int index, char *out, size_t cap)
{
    const char *p, *nl;
    int i;
    size_t len;
    if (!ar || !out || cap == 0) return 0;
    out[0] = '\0';
    p = ar->diagram;
    if (!p[0] || index < 0) return 0;
    for (i = 0; i < index; i++) {
        nl = strchr(p, '\n');
        if (!nl) return 0;
        p = nl + 1;
    }
    nl = strchr(p, '\n');
    len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out[0] ? 1 : 0;
}

static void paint_kb(App *a, HDC dc, RECT content)
{
    RECT y = content;
    int width, needed = 0, i;
    const aic_kb_article *ar;

    app_kb_refresh(a);
    ar = (a->kb && a->kb_article[0]) ? aic_kb_find(a->kb, a->kb_article) : NULL;
    if (!ar) kb_build_rows(a);

    y.left += PAD + 8;
    y.right -= PAD + 8;
    width = (int)(y.right - y.left);

    if (!ar) {
        needed = 52 + 26 + 56 + 12 + 50;
        for (i = 0; i < g_kb_row_n; i++) needed += (g_kb_row_kind[i] ? 36 : 30);
        if (g_kb_row_n == 0) needed += 40;
    } else {
        int pc = aic_kb_paragraph_count(ar);
        needed = 52 + 44 + 12 + adv_measure(dc, a->font_small, ar->summary, width) + 20;
        for (i = 0; i < pc; i++)
            needed += adv_measure(dc, a->font_ui, kb_para_text(ar, i), width) + 16;
        {
            int dn = kb_diagram_count(ar);
            needed += 30 + (dn > 0 ? dn * 34 + (dn - 1) * 18 + 8 : 0) + 16;
        }
        needed += 30 + adv_measure(dc, a->font_small, ar->example, width) + 16;
        needed += 30 + adv_measure(dc, a->font_small, ar->points, width) + 16;
        needed += 30 + ar->source_n * 28 + 24;
        needed += 44;
    }
    {
        int avail = (int)(content.bottom - content.top);
        int maxs = needed - avail;
        if (maxs < 0) maxs = 0;
        if (a->scroll[1] > maxs) a->scroll[1] = maxs;
        if (a->scroll[1] < 0) a->scroll[1] = 0;
    }
    y.top += 8 - a->scroll[1];

    if (!ar) {
        char sub[200];
        draw_text(dc, a->font_title, "内置知识库",
                  (RECT){y.left, y.top, y.right - 140, y.top + 44},
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_pill(a, dc, (RECT){y.right - 120, y.top + 6, y.right, y.top + 42},
                  "返回设置", ACT_KB_BACK, 0);
        y.top += 52;

        if (a->kb)
            snprintf(sub, sizeof(sub), "知识包 v%s · %d 篇 · %d 组 · 书签 %d · 最近阅读 %d",
                     aic_kb_version(a->kb), a->kb->article_n, aic_kb_group_count(a->kb),
                     aic_kb_bookmark_count(a->db), aic_kb_recent_count(a->db));
        else
            aic_strlcpy_utf8(sub, sizeof(sub), "知识包未载入：随包资源缺失或损坏");
        draw_text(dc, a->font_small, sub, (RECT){y.left, y.top, y.right, y.top + 26},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y.top += 26;

        {
            RECT box = {y.left, y.top, y.right - 96, y.top + 44};
            char hint[256];
            fill_round(dc, box, 8, tok(a, AIC_TOK_CARD_BG));
            stroke_round(dc, box, 8, a->kb_focus ? tok(a, AIC_TOK_PRIMARY) : tok(a, AIC_TOK_DIVIDER), 1);
            if (a->kb_query[0])
                aic_strlcpy_utf8(hint, sizeof(hint), a->kb_query);
            else
                aic_strlcpy_utf8(hint, sizeof(hint), a->kb_focus ? "|" : "检索章节标题或正文关键词");
            draw_text(dc, a->font_ui, hint,
                      (RECT){box.left + 12, box.top, box.right - 12, box.bottom},
                      a->kb_query[0] ? tok(a, AIC_TOK_TEXT) : tok(a, AIC_TOK_TEXT_SECONDARY),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            reg(box, ACT_KB_SEARCH_EDIT);
            draw_pill(a, dc, (RECT){y.right - 84, y.top, y.right, y.top + 44},
                      "检索", ACT_KB_SEARCH_GO, 1);
        }
        y.top += 56;

        for (i = 0; i < g_kb_row_n; i++) {
            if (g_kb_row_kind[i] == 0) {
                draw_text(dc, a->font_bold, g_kb_row_label[i],
                          (RECT){y.left, y.top, y.right, y.top + 30},
                          tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                y.top += 30;
            } else {
                RECT rr = {y.left, y.top, y.right, y.top + 36};
                char lab[300];
                snprintf(lab, sizeof(lab), "%s%.200s", g_kb_row_bm[i] ? "★ " : "", g_kb_row_label[i]);
                draw_text(dc, a->font_ui, lab,
                          (RECT){rr.left + 12, rr.top, rr.right - 12, rr.bottom},
                          tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                reg(rr, ACT_KB_ITEM_BASE + i);
                y.top += 36;
            }
        }
        if (g_kb_row_n == 0) {
            draw_text(dc, a->font_small, "没有可显示的条目：知识包为空，或检索没有命中。",
                      (RECT){y.left, y.top, y.right, y.top + 40},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
            y.top += 40;
        }
        y.top += 14;
        draw_text(dc, a->font_small,
                  "章节树随包离线分发，示意图本地存放、不依赖外链；书签与最近阅读只存在本机设置里。"
                  "选中正文段落可插为引用片段，进插件注入区共用同一份上下文预算，片段不携全文。",
                  (RECT){y.left, y.top, y.right, y.top + 52},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
        return;
    }

    draw_text(dc, a->font_title, ar->title,
              (RECT){y.left, y.top, y.right, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y.top += 52;

    draw_pill(a, dc, (RECT){y.left, y.top, y.left + 108, y.top + 34},
              aic_kb_bookmark_has(a->db, ar->id) ? "取消书签" : "加入书签",
              ACT_KB_BOOKMARK, 0);
    draw_pill(a, dc, (RECT){y.left + 116, y.top, y.left + 236, y.top + 34},
              "引用此段", ACT_KB_CITE, 1);
    draw_pill(a, dc, (RECT){y.left + 244, y.top, y.left + 344, y.top + 34},
              "清除引用", ACT_KB_CITE_CLEAR, 0);
    draw_pill(a, dc, (RECT){y.right - 100, y.top, y.right, y.top + 34},
              "返回列表", ACT_KB_BACK, 0);
    y.top += 44;

    draw_text(dc, a->font_small, ar->summary,
              (RECT){y.left, y.top, y.right, y.top + adv_measure(dc, a->font_small, ar->summary, width)},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    y.top += adv_measure(dc, a->font_small, ar->summary, width) + 20;

    {
        int pc = aic_kb_paragraph_count(ar);
        for (i = 0; i < pc; i++) {
            const char *para = kb_para_text(ar, i);
            int h = adv_measure(dc, a->font_ui, para, width);
            RECT rr = {y.left - 6, y.top - 4, y.right, y.top + h + 10};
            if (a->kb_para == i) {
                fill_round(dc, rr, 8, tok(a, AIC_TOK_CARD_BG));
                stroke_round(dc, rr, 8, tok(a, AIC_TOK_PRIMARY), 1);
            }
            draw_text(dc, a->font_ui, para,
                      (RECT){y.left + 4, y.top, y.right - 4, y.top + h},
                      tok(a, AIC_TOK_TEXT), DT_LEFT | DT_TOP | DT_WORDBREAK);
            reg(rr, ACT_KB_PARA_BASE + i);
            y.top += h + 16;
        }
    }

    draw_text(dc, a->font_bold, "示意图（本地绘制，节点顺序即讲解顺序）",
              (RECT){y.left, y.top, y.right, y.top + 26},
              tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 30;
    {
        /* 示意图按「节点 + 箭头」画：节点顺序就是讲解顺序，不依赖任何外链图片。 */
        int dn = kb_diagram_count(ar);
        int ni;
        for (ni = 0; ni < dn; ni++) {
            char node[300];
            RECT nr;
            if (!kb_diagram_node(ar, ni, node, sizeof(node))) break;
            nr.left = y.left + 24;
            nr.right = y.right - 24;
            nr.top = y.top;
            nr.bottom = y.top + 34;
            fill_round(dc, nr, 8, tok(a, AIC_TOK_CARD_BG));
            stroke_round(dc, nr, 8, tok(a, AIC_TOK_PRIMARY), 1);
            draw_text(dc, a->font_ui, node,
                      (RECT){nr.left + 12, nr.top, nr.right - 12, nr.bottom},
                      tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y.top += 34;
            if (ni + 1 < dn) {
                draw_text(dc, a->font_bold, "\u2193",
                          (RECT){y.left, y.top, y.right, y.top + 18},
                          tok(a, AIC_TOK_PRIMARY), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                y.top += 18;
            }
        }
        y.top += 8 + 16;
    }

    draw_text(dc, a->font_bold, "示例", (RECT){y.left, y.top, y.right, y.top + 26},
              tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 30;
    {
        int h = adv_measure(dc, a->font_small, ar->example, width);
        draw_text(dc, a->font_small, ar->example, (RECT){y.left, y.top, y.right, y.top + h},
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_TOP | DT_WORDBREAK);
        y.top += h + 16;
    }

    draw_text(dc, a->font_bold, "要点卡片", (RECT){y.left, y.top, y.right, y.top + 26},
              tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 30;
    {
        int h = adv_measure(dc, a->font_small, ar->points, width);
        draw_text(dc, a->font_small, ar->points, (RECT){y.left, y.top, y.right, y.top + h},
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_TOP | DT_WORDBREAK);
        y.top += h + 16;
    }

    draw_text(dc, a->font_bold, "来源（只收录允许再分发的内容）",
              (RECT){y.left, y.top, y.right, y.top + 26},
              tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 30;
    for (i = 0; i < ar->source_n; i++) {
        char line[512];
        aic_kb_source_line(&ar->sources[i], line, sizeof(line));
        draw_text(dc, a->font_small, line, (RECT){y.left, y.top, y.right, y.top + 26},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y.top += 28;
    }
    y.top += 24;
}


/* ---------- 错误中心（FR-34 / §9.4）----------
 * 错误只写 error_record 与独立 errors/ 目录，不进消息表、不进上下文、不进聊天框；
 * 这里按时间与类别列出来，给重试 / 余额 / 设置 / 能力 / 明细的入口。 */
static void paint_error_center(App *a, HDC dc, RECT content)
{
    RECT y = content;
    int i, cat, width, needed;
    char v[300];

    app_err_reload(a);
    y.left += PAD + 8;
    y.right -= PAD + 8;
    width = (int)(y.right - y.left);

    needed = 52 + 40 + 46 + 14 + a->err_n * 92 + 60;
    if (a->err_n == 0) needed += 40;
    {
        int avail = (int)(content.bottom - content.top);
        int maxs = needed - avail;
        if (maxs < 0) maxs = 0;
        if (a->scroll[1] > maxs) a->scroll[1] = maxs;
        if (a->scroll[1] < 0) a->scroll[1] = 0;
    }
    y.top += 8 - a->scroll[1];

    draw_text(dc, a->font_title, "错误中心",
              (RECT){y.left, y.top, y.right - 140, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_pill(a, dc, (RECT){y.right - 120, y.top + 6, y.right, y.top + 42},
              "返回设置", ACT_ERR_BACK, 0);
    y.top += 52;

    /* 类别筛选：全部 + 八类，窄窗口自动换行。 */
    {
        int per_row = width / 82;
        int px = y.left;
        int py = y.top;
        if (per_row < 1) per_row = 1;
        for (cat = 0; cat <= g_err_cat_n; cat++) {
            RECT rc;
            const char *label = (cat == 0) ? "全部" : aic_error_category_text(g_err_cats[cat - 1]);
            if ((cat % per_row) == 0 && cat > 0) { px = y.left; py += 38; }
            rc = (RECT){px, py, px + 74, py + 32};
            draw_pill(a, dc, rc, label, ACT_ERR_CAT_BASE + cat, a->err_filter == cat);
            px += 82;
        }
        y.top = py + 44;
    }

    /* 操作行：全部已读 / 清除 / 导出诊断包 + 未读与保留期说明。 */
    draw_pill(a, dc, (RECT){y.left, y.top, y.left + 96, y.top + 32},
              "全部已读", ACT_ERR_ACK, 0);
    draw_pill(a, dc, (RECT){y.left + 104, y.top, y.left + 184, y.top + 32},
              "清除", ACT_ERR_CLEAR, 0);
    draw_pill(a, dc, (RECT){y.left + 192, y.top, y.left + 316, y.top + 32},
              "导出诊断包", ACT_ERR_EXPORT, 1);
    snprintf(v, sizeof(v), "%d 条未查看 · 保留 %d 天", aic_error_unread(a->db),
             aic_error_retention_days(a->db));
    draw_text(dc, a->font_small, v,
              (RECT){y.left + 326, y.top, y.right, y.top + 32},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    y.top += 46;

    if (a->err_n == 0) {
        draw_text(dc, a->font_small,
                  aic_error_count(a->db, 0) == 0 ? "还没有记录到运行错误。" : "当前类别下没有错误。",
                  (RECT){y.left, y.top, y.right, y.top + 40},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
        y.top += 40;
    }

    for (i = 0; i < a->err_n; i++) {
        const aic_error_entry *er = &a->err_rows[i];
        RECT card = {y.left, y.top, y.right, y.top + 84};
        char head[300];
        RECT actr;
        int act_w = 96;

        fill_round(dc, card, 8, tok(a, AIC_TOK_CARD_BG));
        snprintf(head, sizeof(head), "%s · %s · %s%s", er->occurred_at,
                 aic_error_category_text(er->category), er->code[0] ? er->code : "未命名错误",
                 er->acknowledged ? "" : " · 未查看");
        draw_text(dc, a->font_small, head,
                  (RECT){card.left + 12, card.top + 8, card.right - act_w - 24, card.top + 30},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(dc, a->font_ui, er->message,
                  (RECT){card.left + 12, card.top + 30, card.right - act_w - 24, card.top + 54},
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (er->detail_path[0]) {
            /* 右侧已经有动作入口，这里只说「留了脱敏原始响应」，不重复动作名。 */
            draw_text(dc, a->font_small, "已留脱敏原始响应（可在诊断包里导出）",
                      (RECT){card.left + 12, card.top + 56, card.right - act_w - 24, card.top + 78},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        actr = (RECT){card.right - act_w - 8, card.top + 24, card.right - 8, card.top + 60};
        draw_pill(a, dc, actr, aic_error_action_text(aic_error_action_for(er->category)),
                  ACT_ERR_ACT_BASE + i, 0);
        y.top += 92;
    }

    y.top += 10;
    draw_text(dc, a->font_small,
              "原始响应脱敏后写进 errors/ 目录（诊断日志）；错误不进消息表、不进上下文，也不进聊天框。"
              "额度不足给余额入口、网络中断给重试入口，重试沿用输入框里的原草稿，不产生新消息。",
              (RECT){y.left, y.top, y.right, y.top + 56},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
}

/* ---------- 设备能力与权限（FR-35 / §18） ---------- */
/* 「选择…」按钮的动作：只有设备图片 / 已安装软件 / 本地文件三项需要用户显式挑一条。 */
static int cap_pick_action(const char *kind)
{
    if (strcmp(kind, AIC_CAP_MEDIA) == 0) return ACT_CAP_PICK_MEDIA;
    if (strcmp(kind, AIC_CAP_APPS) == 0) return ACT_CAP_PICK_APPS;
    if (strcmp(kind, AIC_CAP_FILES) == 0) return ACT_CAP_PICK_FILES;
    return 0;
}

/* 选一个本机文件，把路径写进设置键；取消返回 0。 */
static int cap_pick_path(App *a, const wchar_t *filter, const char *key, const wchar_t *title)
{
    OPENFILENAMEW ofn;
    wchar_t wf[900];
    char u8[900];

    wf[0] = 0;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = a->hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = wf;
    ofn.nMaxFile = (DWORD)(sizeof(wf) / sizeof(wf[0]));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    w_to_u8(wf, u8, (int)sizeof(u8));
    aic_setting_set(a->db, key, u8);
    app_publish_setting(a, key);
    return 1;
}

static void paint_capability(App *a, HDC dc, RECT content)
{
    RECT y = content;
    int i, n = (int)aic_cap_count(), needed;
    char v[300];

    y.left += PAD + 8;
    y.right -= PAD + 8;

    needed = 52 + 30 + n * 104 + 20 + 80;
    if (a->app_n > 0) needed += 40 + a->app_n * 34;
    {
        int avail = (int)(content.bottom - content.top);
        int maxs = needed - avail;
        if (maxs < 0) maxs = 0;
        if (a->scroll[1] > maxs) a->scroll[1] = maxs;
        if (a->scroll[1] < 0) a->scroll[1] = 0;
    }
    y.top += 8 - a->scroll[1];

    draw_text(dc, a->font_title, "设备权限",
              (RECT){y.left, y.top, y.right - 140, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_pill(a, dc, (RECT){y.right - 120, y.top + 6, y.right, y.top + 42},
              "返回设置", ACT_CAP_BACK, 0);
    y.top += 52;

    snprintf(v, sizeof(v), "时间固定授予、不可撤销，未授予不进主页；其余按需申请，可单独授予或撤销。");
    draw_text(dc, a->font_small, v, (RECT){y.left, y.top, y.right, y.top + 26},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 30;

    for (i = 0; i < n; i++) {
        const char *kind = aic_cap_kind_at(i);
        aic_cap_status st;
        RECT card = {y.left, y.top, y.right, y.top + 96};
        const char *state_txt;
        int revocable;

        if (!kind) continue;
        memset(&st, 0, sizeof(st));
        aic_cap_status_load(a->db, kind, &st);
        revocable = aic_cap_revocable(kind);

        fill_round(dc, card, 8, tok(a, AIC_TOK_CARD_BG));
        draw_text(dc, a->font_ui, aic_cap_name(kind),
                  (RECT){card.left + 12, card.top + 8, card.left + 160, card.top + 32},
                  tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (st.granted) state_txt = aic_cap_required(kind) ? "已授予（必需）" : "已授予";
        else if (strcmp(st.state, "denied") == 0) state_txt = "已拒绝";
        else if (strcmp(st.state, "unset") == 0) state_txt = "未申请";
        else state_txt = st.state;
        draw_text(dc, a->font_small, state_txt,
                  (RECT){card.left + 168, card.top + 8, card.right - 140, card.top + 32},
                  st.granted ? tok(a, AIC_TOK_SUCCESS) : tok(a, AIC_TOK_TEXT_SECONDARY),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        draw_text(dc, a->font_small, aic_cap_purpose(kind),
                  (RECT){card.left + 12, card.top + 34, card.right - 140, card.top + 58},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (aic_cap_required(kind) && !revocable) {
            draw_pill(a, dc, (RECT){card.right - 132, card.top + 8, card.right - 12, card.top + 40},
                      "必需", 0, 0);
        } else {
            draw_pill(a, dc, (RECT){card.right - 132, card.top + 8, card.right - 12, card.top + 40},
                      st.granted ? "撤销" : "授予", ACT_CAP_TOGGLE_BASE + i, !st.granted);
            if (cap_pick_action(kind))
                draw_pill(a, dc, (RECT){card.right - 132, card.top + 48, card.right - 12, card.top + 80},
                          "选择…", cap_pick_action(kind), 0);
        }

        if (st.last_used_at[0]) {
            char used[80];
            snprintf(used, sizeof(used), "最近使用 %s", st.last_used_at);
            draw_text(dc, a->font_small, used,
                      (RECT){card.left + 12, card.top + 60, card.right - 140, card.top + 82},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (st.deny_count > 0) {
            snprintf(v, sizeof(v), "已拒绝 %d 次（不影响主对话）", st.deny_count);
            draw_text(dc, a->font_small, v,
                      (RECT){card.left + 12, card.top + 60, card.right - 140, card.top + 82},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        y.top += 104;
    }

    if (a->app_n > 0) {
        y.top += 6;
        draw_text(dc, a->font_bold, "选一条作为「已安装软件」的注入内容（完整清单不上传）",
                  (RECT){y.left, y.top, y.right, y.top + 26},
                  tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y.top += 28;
        for (i = 0; i < a->app_n; i++) {
            RECT rr = {y.left, y.top, y.right, y.top + 34};
            char sel[160];
            int cur;
            sel[0] = '\0';
            if (aic_setting_get(a->db, AIC_SET_CAP_PREFIX "apps.selected", sel, sizeof(sel)) != AIC_OK)
                sel[0] = '\0';
            cur = aic_str_ieq(sel, a->app_list[i]);
            draw_text(dc, a->font_small, a->app_list[i],
                      (RECT){rr.left + 12, rr.top, rr.right - 12, rr.bottom},
                      cur ? tok(a, AIC_TOK_PRIMARY) : tok(a, AIC_TOK_TEXT),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            reg(rr, ACT_CAP_APP_BASE + i);
            y.top += 34;
        }
        y.top += 14;
    }

    draw_text(dc, a->font_small,
              "取数都在本机做：位置与天气在桌面端没有可用来源，取不到时按 §9.4 写一条错误，主对话与同步继续可用；"
              "原始图片、原始文件、完整软件清单都不上传，注入前脱敏。",
              (RECT){y.left, y.top, y.right, y.top + 52},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (a->cap_msg[0]) {
        y.top += 56;
        draw_text(dc, a->font_small, a->cap_msg,
                  (RECT){y.left, y.top, y.right, y.top + 40},
                  tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

/* ---------- 首次启动：API Key 配置（FR-30 / §2.3 / §4.4） ---------- */
/* 单行输入框的显示：空且未聚焦时画提示语，聚焦时补一个光标；masked 用于凭证。 */
static void draw_field(App *a, HDC dc, RECT box, const char *text, int focus, int masked,
                       const char *hint)
{
    char vis[512];
    size_t n = strlen(text ? text : "");

    fill_round(dc, box, 8, tok(a, AIC_TOK_CARD_BG));
    stroke_round(dc, box, 8, focus ? tok(a, AIC_TOK_PRIMARY) : tok(a, AIC_TOK_DIVIDER), 1);
    if (n == 0) {
        aic_strlcpy_utf8(vis, sizeof(vis), focus ? "|" : hint);
    } else if (masked) {
        size_t i, cap = sizeof(vis);
        for (i = 0; i < n && i + 2 < cap; i++) vis[i] = '*';
        if (focus && i + 2 < cap) vis[i++] = '|';
        vis[i] = 0;
    } else {
        aic_strlcpy_utf8(vis, sizeof(vis), text);
        if (focus) {
            size_t l = strlen(vis);
            if (l + 2 < sizeof(vis)) { vis[l] = '|'; vis[l + 1] = 0; }
        }
    }
    draw_text(dc, a->font_ui, vis, (RECT){box.left + 12, box.top, box.right - 12, box.bottom},
              (n || focus) ? tok(a, AIC_TOK_TEXT) : tok(a, AIC_TOK_TEXT_SECONDARY),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void paint_credential(App *a, HDC dc, RECT client)
{
    RECT t = {client.left + 40, client.top + 66, client.right - 40, client.top + 112};
    RECT d = {client.left + 40, client.top + 116, client.right - 60, client.top + 216};
    RECT lbl = {client.left + 40, client.top + 232, client.right - 40, client.top + 256};
    RECT box = {client.left + 40, client.top + 258, client.right - 40, client.top + 298};
    RECT b1 = {client.left + 40, client.top + 316, client.left + 190, client.top + 356};
    RECT b2 = {client.left + 206, client.top + 316, client.left + 346, client.top + 356};

    draw_text(dc, a->font_title, "配置 DeepSeek API Key", t, tok(a, AIC_TOK_TEXT),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, a->font_ui,
              "首次启动必须先填写 API Key。凭证只写入系统安全存储（Windows 凭据管理器），"
              "界面与日志不出现明文，导出包与备份包也不含凭证；凭证就绪后才进入主页选择默认模式。"
              " 没有 Key 时可以先用「演示模式」看界面，演示不会发出任何网络请求。",
              d, tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, a->font_small, "API Key（形如 sk-...）", lbl,
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_field(a, dc, box, a->cred_key, a->cred_focus, 1, "点这里输入");
    reg(box, ACT_CRED_KEY_EDIT);
    draw_pill(a, dc, b1, "校验并继续", ACT_CRED_SAVE, 1);
    draw_pill(a, dc, b2, "演示模式", ACT_CRED_DEMO, 0);
}

/* ---------- 首次启动：默认模式选择（FR-30） ---------- */
static void paint_mode_select(App *a, HDC dc, RECT client)
{
    RECT t = {client.left + 40, client.top + 66, client.right - 40, client.top + 112};
    RECT d = {client.left + 40, client.top + 116, client.right - 60, client.top + 176};
    RECT c1 = {client.left + 40, client.top + 200, client.left + 400, client.top + 358};
    RECT c2 = {client.left + 420, client.top + 200, client.left + 780, client.top + 358};
    RECT b1 = {client.left + 60, client.top + 306, client.left + 250, client.top + 346};
    RECT b2 = {client.left + 440, client.top + 306, client.left + 630, client.top + 346};

    draw_text(dc, a->font_title, "选择默认模式", t, tok(a, AIC_TOK_TEXT),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, a->font_ui,
              "两种模式共用凭证、模型、人设、插件与用户资料，会话记录按模式隔离；"
              "同一时刻只有一个模式活动，之后可以在左下角随时切换，各自保留滚动位置与草稿。",
              d, tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);

    fill_round(dc, c1, 12, tok(a, AIC_TOK_CARD_BG));
    stroke_round(dc, c1, 12, tok(a, AIC_TOK_DIVIDER), 1);
    draw_text(dc, a->font_bold, "聊天模式", (RECT){c1.left + 20, c1.top + 16, c1.right - 20, c1.top + 48},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, a->font_small,
              "单对话、QQ 风格的三栏界面，适合长期陪伴。默认进入「聊天」栏。",
              (RECT){c1.left + 20, c1.top + 52, c1.right - 20, c1.top + 118},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_pill(a, dc, b1, "用聊天模式", ACT_MODE_CHAT, 1);

    fill_round(dc, c2, 12, tok(a, AIC_TOK_CARD_BG));
    stroke_round(dc, c2, 12, tok(a, AIC_TOK_DIVIDER), 1);
    draw_text(dc, a->font_bold, "问答模式", (RECT){c2.left + 20, c2.top + 16, c2.right - 20, c2.top + 48},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, a->font_small,
              "多会话列表、置顶与搜索，参照 DeepSeek 网页版的工作方式，适合查资料与写代码。",
              (RECT){c2.left + 20, c2.top + 52, c2.right - 20, c2.top + 118},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_pill(a, dc, b2, "用问答模式", ACT_MODE_QA, 1);
}

/* ---------- 配对管理页（第 16 章 FR-25 / FR-26 / FR-27 / FR-28） ---------- */
static const char *pair_phase_cn(aic_sync_state st)
{
    switch (st) {
    case AIC_SYNC_DISCOVERING: return "发现中";
    case AIC_SYNC_CONNECTING:  return "连接中";
    case AIC_SYNC_PAIRING:     return "待配对（正在等对端输入配对码）";
    case AIC_SYNC_PAIRED:      return "已配对（链路已断开，事件留在本地队列）";
    case AIC_SYNC_SYNCING:     return "同步中";
    default:                   return "空闲";
    }
}

static void paint_pair(App *a, HDC dc, RECT content)
{
    RECT y = content;
    char v[320];
    const char *code = a->ses ? aic_session_our_code(a->ses) : "";
    aic_sync_state ph = a->ses ? aic_session_phase(a->ses) : AIC_SYNC_IDLE;
    int paired = a->ses ? aic_session_is_paired(a->ses) : 0;
    aic_pair_record rec;
    int have_rec = (aic_pairing_current(a->db, &rec) == AIC_OK);
    int conflicts = 0;
    int outbox = 0;
    int realtime = aic_sync_realtime_enabled(a->db);

    aic_sync_conflict_count(a->db, &conflicts);
    aic_sync_outbox_count(a->db, &outbox);

    y.left += PAD + 8;
    y.right -= PAD + 8;
    {
        int needed = 700 + a->peer_n * 44;
        int avail = (int)(content.bottom - content.top);
        int maxs = needed - avail;
        if (maxs < 0) maxs = 0;
        if (a->scroll[1] > maxs) a->scroll[1] = maxs;
        if (a->scroll[1] < 0) a->scroll[1] = 0;
    }
    y.top += 8 - a->scroll[1];

    draw_text(dc, a->font_title, "配对管理", (RECT){y.left, y.top, y.right - 140, y.top + 44},
              tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_pill(a, dc, (RECT){y.right - 120, y.top + 6, y.right, y.top + 42}, "返回设置", ACT_PAIR_BACK, 0);
    y.top += 52;

    /* 本端配对码 */
    {
        RECT card = {y.left, y.top, y.right, y.top + 108};
        fill_round(dc, card, 12, tok(a, AIC_TOK_CARD_BG));
        stroke_round(dc, card, 12, tok(a, AIC_TOK_DIVIDER), 1);
        draw_text(dc, a->font_small, "本端配对码（有效期 5 分钟，出错 5 次作废）",
                  (RECT){card.left + 20, card.top + 10, card.right - 20, card.top + 34},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (code && *code)
            draw_text(dc, a->font_title, code,
                      (RECT){card.left + 20, card.top + 34, card.right - 20, card.top + 78},
                      tok(a, AIC_TOK_PRIMARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        else
            draw_text(dc, a->font_bold, "尚未生成：点下面的「开始监听等待连入」或「连到该地址并配对」",
                      (RECT){card.left + 20, card.top + 34, card.right - 20, card.top + 78},
                      tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        snprintf(v, sizeof(v), "状态：%s%s%s", pair_phase_cn(ph), paired ? " · 已与 " : "",
                 paired && have_rec ? rec.peer_name : "");
        draw_text(dc, a->font_small, v,
                  (RECT){card.left + 20, card.top + 80, card.right - 20, card.top + 104},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y.top += 120;
    }

    draw_text(dc, a->font_small,
              "两端各自生成 6 位码：连接方输入的码必须等于被连接方生成的值，被连接方输入同理，"
              "两边都填对才配对成立；任一端输错即终止本次配对，配对完成前不交换任何业务实体。"
              " 配对成功后实时同步默认开启；同一时刻只与一个已配对设备驻留。",
              (RECT){y.left, y.top, y.right, y.top + 64},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
    y.top += 72;

    /* 对端码输入 + 握手 */
    draw_text(dc, a->font_small, "对端显示的 6 位配对码",
              (RECT){y.left, y.top, y.right, y.top + 22},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 24;
    {
        RECT box = {y.left, y.top, y.left + 200, y.top + 38};
        RECT b = {y.left + 216, y.top, y.left + 400, y.top + 38};
        draw_field(a, dc, box, a->pair_code_in, a->pair_focus == 1, 0, "例如 123456");
        reg(box, ACT_PAIR_CODE_EDIT);
        draw_pill(a, dc, b, "完成配对", ACT_PAIR_HANDSHAKE, 1);
        y.top += 48;
    }

    /* 地址输入 + 连接 */
    draw_text(dc, a->font_small, "对端局域网地址（也可以直接点下面发现的设备）",
              (RECT){y.left, y.top, y.right, y.top + 22},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 24;
    {
        RECT box = {y.left, y.top, y.left + 260, y.top + 38};
        RECT b1 = {y.left + 276, y.top, y.left + 500, y.top + 38};
        draw_field(a, dc, box, a->pair_addr_in, a->pair_focus == 2, 0, "例如 192.168.1.8");
        reg(box, ACT_PAIR_ADDR_EDIT);
        draw_pill(a, dc, b1, "连到该地址并配对", ACT_PAIR_CONNECT, 1);
        y.top += 48;
    }

    /* 两个入口：监听与发现 */
    {
        RECT b1 = {y.left, y.top, y.left + 220, y.top + 38};
        RECT b2 = {y.left + 236, y.top, y.left + 440, y.top + 38};
        draw_pill(a, dc, b1, a->pair_channel == AIC_CH_BLUETOOTH
                                   ? "开始监听等待连入（蓝牙）" : "开始监听等待连入", ACT_PAIR_HOST, 0);
        draw_pill(a, dc, b2, a->pair_channel == AIC_CH_BLUETOOTH
                                   ? "列出已配对蓝牙设备" : "发现同网设备", ACT_PAIR_DISCOVER, 0);
        y.top += 50;
        {
            RECT c1 = {y.left, y.top, y.left + 300, y.top + 34};
            RECT c2 = {y.left + 316, y.top, y.left + 616, y.top + 34};
            int cur = app_pair_channel(a);
            draw_pill(a, dc, c1, cur == AIC_CH_LAN ? "通道：局域网（当前）" : "通道：局域网",
                      ACT_PAIR_CH_LAN, 0);
            draw_pill(a, dc, c2, cur == AIC_CH_BLUETOOTH ? "通道：蓝牙 RFCOMM（当前）" : "通道：蓝牙 RFCOMM",
                      ACT_PAIR_CH_BT, 0);
            y.top += 44;
        }
    }

    if (a->peer_n > 0) {
        draw_text(dc, a->font_small, "发现的设备（点一行填入地址）",
                  (RECT){y.left, y.top, y.right, y.top + 22},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y.top += 24;
        for (int i = 0; i < a->peer_n && i < (int)(sizeof(a->peers) / sizeof(a->peers[0])); i++) {
            RECT row = {y.left, y.top, y.right, y.top + 36};
            fill_round(dc, row, 8, tok(a, AIC_TOK_CARD_BG));
            snprintf(v, sizeof(v), "%s · %s:%d", a->peers[i].name, a->peers[i].addr, a->peers[i].port);
            draw_text(dc, a->font_ui, v, (RECT){row.left + 12, row.top, row.right - 12, row.bottom},
                      tok(a, AIC_TOK_TEXT), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            reg(row, ACT_PAIR_DISCOVER + 100 + i);
            y.top += 44;
        }
    }

    /* 同步开关与冲突 */
    {
        RECT b1 = {y.left, y.top, y.left + 200, y.top + 38};
        RECT b2 = {y.left + 216, y.top, y.left + 420, y.top + 38};
        draw_pill(a, dc, b1, realtime ? "暂停实时同步" : "恢复实时同步", ACT_PAIR_TOGGLE, 0);
        snprintf(v, sizeof(v), "查看冲突（%d）", conflicts);
        draw_pill(a, dc, b2, v, ACT_PAIR_CONFLICTS, 0);
        y.top += 44;
    }
    snprintf(v, sizeof(v), "待发送事件 %d 条 · 同步范围：用户资料 / 人设 / 会话 / 消息 / 提示词版本 / 设置白名单 / 插件清单",
             outbox);
    draw_text(dc, a->font_small, v, (RECT){y.left, y.top, y.right, y.top + 22},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y.top += 30;

    if (have_rec) {
        RECT b = {y.left, y.top, y.left + 160, y.top + 38};
        draw_pill(a, dc, b, "解除配对", ACT_PAIR_UNPAIR, 0);
        y.top += 46;
    }

    if (a->pair_msg[0]) {
        draw_text(dc, a->font_small, a->pair_msg,
                  (RECT){y.left, y.top, y.right, y.top + 60},
                  tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
        y.top += 66;
    }
    draw_text(dc, a->font_small,
              "两条通道共用同一套帧格式与配对流程：局域网用于同一网段的两台设备，蓝牙 RFCOMM 用于"
              "已系统配对的设备（Android 由平台套接字转发字节流）。跨网络或防火墙阻断时请改用导出导入迁移记录。",
              (RECT){y.left, y.top, y.right, y.top + 40},
              tok(a, AIC_TOK_TEXT_SECONDARY), DT_LEFT | DT_TOP | DT_WORDBREAK);
}

/* ---------- 输入事件 ---------- */
static void on_click(App *a, int x, int y)
{
    int act = hit_test(x, y);
    a->input_focus = 0;
    if (act == ACT_SEND) {
        /* 点输入框聚焦，点发送按钮发送：发送按钮区域后注册，命中优先。 */
        RECT box_check = {0, 0, 0, 0};
        (void)box_check;
    }
    if (act == ACT_NONE) {
        app_invalidate(a);
        return;
    }
    do_action(a, act);
}

static void on_char(App *a, wchar_t ch)
{
    if (ch < 32) return;
    if (a->cred_focus) {
        edit_append(a->cred_key, sizeof(a->cred_key), &ch, 1);
        app_invalidate(a);
        return;
    }
    if (a->page_pair && a->pair_focus) {
        if (a->pair_focus == 1) {
            if (ch >= '0' && ch <= '9') edit_append(a->pair_code_in, sizeof(a->pair_code_in), &ch, 1);
        } else {
            edit_append(a->pair_addr_in, sizeof(a->pair_addr_in), &ch, 1);
        }
        app_invalidate(a);
        return;
    }
    if (a->page_kb && a->kb_focus) {
        edit_append(a->kb_query, sizeof(a->kb_query), &ch, 1);
        app_invalidate(a);
        return;
    }
    if (a->overlay != AIC_OV_NONE && a->field_focus >= 0 && a->field_focus < a->field_n) {
        Field *fl = &a->fields[a->field_focus];
        edit_append(fl->buf, sizeof(fl->buf), &ch, 1);
        a->form_dirty = 1;
        app_invalidate(a);
        return;
    }
    if (a->search_focus) {
        edit_append(a->search, sizeof(a->search), &ch, 1);
        app_reload_threads(a);
        app_invalidate(a);
        return;
    }
    if (aic_router_state(a->router) == AIC_ST_FIRST_RUN ||
        aic_router_state(a->router) == AIC_ST_WIZARD) {
        return;
    }
    edit_append(a->draft, sizeof(a->draft), &ch, 1);
    a->draft_len = (int)strlen(a->draft);
    a->input_focus = 1;
    app_invalidate(a);
}

static void app_paste(App *a)
{
    HANDLE h;
    const wchar_t *w;
    if (!OpenClipboard(a->hwnd)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        w = (const wchar_t *)GlobalLock(h);
        if (w) {
            if (a->overlay != AIC_OV_NONE && a->field_focus >= 0 && a->field_focus < a->field_n) {
                edit_append(a->fields[a->field_focus].buf, sizeof(a->fields[0].buf),
                            w, (int)wcslen(w));
                a->form_dirty = 1;
            }
            else if (a->page_kb && a->kb_focus) {
                edit_append(a->kb_query, sizeof(a->kb_query), w, (int)wcslen(w));
            }
            else if (a->search_focus) {
                edit_append(a->search, sizeof(a->search), w, (int)wcslen(w));
                app_reload_threads(a);
            } else {
                edit_append(a->draft, sizeof(a->draft), w, (int)wcslen(w));
                a->draft_len = (int)strlen(a->draft);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    app_invalidate(a);
}

static void on_key(App *a, int vk)
{
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (a->cred_focus) {
        if (vk == VK_BACK) edit_backspace(a->cred_key, sizeof(a->cred_key));
        else if (vk == VK_RETURN) { app_cred_save(a); return; }
        else if (vk == VK_ESCAPE) a->cred_focus = 0;
        app_invalidate(a);
        return;
    }
    if (a->page_pair && a->pair_focus) {
        if (vk == VK_BACK) {
            if (a->pair_focus == 1) edit_backspace(a->pair_code_in, sizeof(a->pair_code_in));
            else edit_backspace(a->pair_addr_in, sizeof(a->pair_addr_in));
        } else if (vk == VK_RETURN) {
            app_pair_start(a, 4);
            return;
        } else if (vk == VK_ESCAPE) {
            a->pair_focus = 0;
        }
        app_invalidate(a);
        return;
    }

    if (ctrl && (vk == 'V' || vk == 'v')) {
        app_paste(a);
        return;
    }
    switch (vk) {
    case VK_ESCAPE:
        if (a->page_kb && a->kb_focus) {
            a->kb_focus = 0;
        } else if (a->page_kb) {
            if (a->kb_article[0]) {
                a->kb_article[0] = '\0';
                a->kb_para = -1;
                a->kb_hit_n = 0;
                a->scroll[1] = 0;
            } else {
                cap_leave_subpage(a);
            }
        } else if (a->page_error || a->page_cap) {
            cap_leave_subpage(a);
        } else if (a->overlay != AIC_OV_NONE) {
            app_try_close(a);
        } else if (a->search_focus) {
            a->search_focus = 0;
            a->search[0] = '\0';
            app_reload_threads(a);
        } else {
            aic_back_action ba = aic_router_back(a->router);
            if (ba == AIC_BACK_EXIT_CONFIRM) {
                a->quit_confirm = 1;
                app_set_overlay(a, AIC_OV_CONFIRM, "退出 Sai");
                a->confirm_action = FORM_CONFIRM;
                snprintf(a->overlay_text, sizeof(a->overlay_text),
                         "确定退出吗？聊天记录与草稿都会保留。");
            }
            app_load_chat(a);
        }
        return;
    case VK_BACK:
        if (a->overlay != AIC_OV_NONE && a->field_focus >= 0 && a->field_focus < a->field_n)
            edit_backspace(a->fields[a->field_focus].buf, sizeof(a->fields[0].buf));
        else if (a->page_kb && a->kb_focus)
            edit_backspace(a->kb_query, sizeof(a->kb_query));
        else if (a->search_focus) {
            edit_backspace(a->search, sizeof(a->search));
            app_reload_threads(a);
        } else {
            edit_backspace(a->draft, sizeof(a->draft));
            a->draft_len = (int)strlen(a->draft);
        }
        break;
    case VK_RETURN:
        if (a->overlay != AIC_OV_NONE) {
            if (a->field_focus >= 0 && a->field_focus < a->field_n &&
                a->fields[a->field_focus].multiline) {
                Field *fl = &a->fields[a->field_focus];
                edit_append(fl->buf, sizeof(fl->buf), L"\n", 1);
            } else if (a->field_focus + 1 < a->field_n) {
                a->field_focus++;
            } else {
                app_save_form(a);
                return;
            }
        } else if (a->page_kb && a->kb_focus) {
            kb_search_run(a);
        } else if (a->search_focus) {
            a->search_focus = 0;
        } else {
            app_send(a);
        }
        break;
    case VK_TAB:
        if (a->field_n > 0) {
            a->field_focus = (a->field_focus + 1) % a->field_n;
            a->input_focus = 0;
        }
        break;
    default:
        break;
    }
    app_invalidate(a);
}

static void on_wheel(App *a, int delta)
{
    int step = delta / WHEEL_DELTA;
    if (a->overlay != AIC_OV_NONE) return;
    if (aic_router_mode(a->router) == AIC_MODE_QA ||
        aic_router_tab(a->router) == AIC_TAB_CHAT) {
        a->scroll[2] += step * 48;
        if (a->scroll[2] < 0) a->scroll[2] = 0;
    } else {
        int t = (int)aic_router_tab(a->router);
        if (t < 0 || t >= 3) t = 0;
        a->scroll[t] -= step * 48;
        if (a->scroll[t] < 0) a->scroll[t] = 0;
    }
    app_invalidate(a);
}

/* ---------- 初始化与收尾 ---------- */
static int app_root_dir(App *a)
{
    const char *base = getenv("SAI_DATA_DIR");
    static const char *subs[] = {"db", "plugins", "avatars", "backups", "exports", "logs", "updates"};

    if (base && *base) {
        /* 便携模式：数据目录由环境变量指定（便携包与验收测试用）。 */
        aic_strlcpy_utf8(a->root, sizeof(a->root), base);
        aic_mkdir_p(a->root);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            char p[800];
            aic_path_join(p, sizeof(p), a->root, subs[i]);
            aic_mkdir_p(p);
        }
        return AIC_OK;
    }
    base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = ".";
    aic_path_join(a->root, sizeof(a->root), base, "Sai");
    aic_mkdir_p(a->root);
    for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
        char p[800];
        aic_path_join(p, sizeof(p), a->root, subs[i]);
        aic_mkdir_p(p);
    }
    return AIC_OK;
}

static void app_init(App *a)
{
    HDC screen;

    /* SAI_DEMO=1 时用本地假传输演示对话（不发任何请求），便于离线看界面。 */
    if (getenv("SAI_DEMO")) a->offline_demo = 1;
    memset(a->scroll, 0, sizeof(a->scroll));
    a->field_focus = -1;
    a->input_focus = 1;

    screen = GetDC(NULL);
    a->dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (a->dpi < 96) a->dpi = 96;

    a->font_ui = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    a->font_title = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    a->font_bold = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    a->font_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    /* 代码块用等宽字体（技术设计 6.1）。 */
    a->font_mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");

    app_root_dir(a);
    DLOG("[diag] root=%s", a->root);
    {
        int rc_db = app_open_db(a);
        DLOG("[diag] open_db rc=%d", rc_db);
        if (rc_db != AIC_OK) {
            DLOG("[diag] db open failed, data dir=%s", a->root);
            if (!g_diag)
                MessageBoxW(a->hwnd, L"打开本地数据库失败，请检查数据目录的写入权限。",
                            L"Sai", MB_ICONERROR | MB_OK);
            return;
        }
    }
    if (aic_router_create(a->db, &a->router) != AIC_OK) { DLOG("[diag] router fail"); return; }
    aic_router_boot(a->router);
    aic_router_config_check(a->router);
    if (a->offline_demo) {
        /* 演示模式：不写凭证，只放行「凭证 → 模式选择」的顺序，便于离线看界面。 */
        aic_route_state st = aic_router_state(a->router);
        if (st == AIC_ST_CREDENTIAL_CHECK || st == AIC_ST_CREDENTIAL_SETUP || st == AIC_ST_MODE_SELECT) {
            aic_setting_set(a->db, AIC_SET_CRED_HAS_KEY, "1");
            aic_setting_set_int(a->db, AIC_SET_FIRST_RUN_DONE, 1);
            aic_setting_set(a->db, AIC_SET_DEFAULT_MODE, "chat");
            aic_router_config_check(a->router);
        }
    }
    if (aic_router_state(a->router) == AIC_ST_FIRST_RUN) aic_router_start_wizard(a->router);
    DLOG("[diag] router state=%s", aic_route_state_name(aic_router_state(a->router)));

    {
        char ldir[600];
        aic_path_join(ldir, sizeof(ldir), a->root, "logs");
        aic_mkdir_p(ldir);
        aic_log_init(ldir);
        DLOG("[diag] log dir=%s", ldir);
    }
    {
        /* 技术设计 9.3：新增模型通过配置文件追加，不改核心代码。 */
        char mfile[800];
        char merr[256] = {0};
        aic_path_join(mfile, sizeof(mfile), a->root, "models.json");
        if (aic_path_exists(mfile)) {
            if (aic_llm_models_load_file(mfile, merr, sizeof(merr)) == AIC_OK)
                DLOG("[diag] models.json loaded");
            else
                DLOG("[diag] models.json invalid: %s", merr);
        }
    }
    a->system_night = read_system_night();
    app_setup_engine(a);
    DLOG("[diag] engine=%p", (void *)a->eng);
    app_reload_threads(a);
    app_load_chat(a);
    DLOG("[diag] chat conv=%s msgs=%d", a->conv_id, (int)a->msg_n);
    a->scroll[2] = 0;
    a->caret_tick = aic_now_ms();

    /* 第 16 章：同步搬运线程（网络与数据库读取不占用界面线程）。 */
    app_sync_start(a);

    /* §14.1：启动时检查一次（可在设置里关闭），之后按周期检查。 */
    if (aic_setting_get_int(a->db, AIC_SET_UPDATE_AUTO, 1)) {
        long long last = (long long)aic_setting_get_int(a->db, "update.last_check_ms", 0);
        int hours = aic_setting_get_int(a->db, AIC_SET_UPDATE_INTERVAL, 24);
        if (aic_update_should_check(last, hours, aic_now_ms())) app_check_update(a, 0);
    }
}

static void app_shutdown(App *a)
{
    if (a->msgs) aic_msg_free_array(a->msgs, a->msg_n);
    a->msgs = NULL;
    a->msg_n = 0;
    if (a->threads) aic_conv_free(a->threads, a->thread_n);
    a->threads = NULL;
    a->thread_n = 0;
    if (a->kb) { aic_kb_free(a->kb); a->kb = NULL; }
    aic_cap_clear_provider();
    if (a->eng) aic_engine_destroy(a->eng);
    a->eng = NULL;
    app_sync_stop(a);
    if (a->ses) aic_session_destroy(a->ses);
    a->ses = NULL;
    if (a->router) aic_router_destroy(a->router);
    a->router = NULL;
    if (a->db) aic_db_close(a->db);
    a->db = NULL;
    if (a->font_ui) DeleteObject(a->font_ui);
    if (a->font_title) DeleteObject(a->font_title);
    if (a->font_bold) DeleteObject(a->font_bold);
    if (a->font_small) DeleteObject(a->font_small);
    a->font_ui = a->font_title = a->font_bold = a->font_small = NULL;
}

/* ---------- 离屏渲染截图（诊断，--shot 用） ---------- */
static int app_shoot(App *a, int w, int h, const char *path)
{
    HDC sdc, mdc;
    HBITMAP bm;
    HGDIOBJ old;
    RECT rc;
    BITMAPINFO bi;
    int stride = w * 4;
    unsigned char *px;
    int ok = 0;
    FILE *fp;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;

    if (!path || !path[0] || w <= 0 || h <= 0) return AIC_E_INVAL;
    sdc = GetDC(NULL);
    if (!sdc) return AIC_E_IO;
    mdc = CreateCompatibleDC(sdc);
    bm = CreateCompatibleBitmap(sdc, w, h);
    if (!mdc || !bm) { if (mdc) DeleteDC(mdc); if (bm) DeleteObject(bm); ReleaseDC(NULL, sdc); return AIC_E_NOMEM; }
    old = SelectObject(mdc, bm);
    rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = h;
    paint(a, mdc, rc);
    SelectObject(mdc, old);
    DeleteDC(mdc);

    px = (unsigned char *)malloc((size_t)stride * (size_t)h);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (px && GetDIBits(sdc, bm, 0, (UINT)h, px, &bi, DIB_RGB_COLORS) == (int)h) ok = 1;
    ReleaseDC(NULL, sdc);
    DeleteObject(bm);
    if (!ok) { aic_xfree(px); return AIC_E_IO; }

    fp = fopen(path, "wb");
    if (!fp) { aic_xfree(px); return AIC_E_IO; }
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)stride * (DWORD)h;
    memset(&ih, 0, sizeof(ih));
    ih.biSize = sizeof(BITMAPINFOHEADER);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)stride * (DWORD)h;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&ih, sizeof(ih), 1, fp);
    fwrite(px, 1, (size_t)stride * (size_t)h, fp);
    fclose(fp);
    aic_xfree(px);
    return AIC_OK;
}

/* ---------- 截图模式（--shot）的状态准备 ---------- */
static char g_shot_path[512];
static int  g_shot_tab = -1;
static int  g_shot_overlay = -1;
static int  g_shot_demo = 0;
static int  g_shot_qa = 0;
static int  g_shot_guide = 0;
static int  g_shot_adv = 0;
static int  g_shot_ready = 0;
static int  g_shot_cred = 0;
static int  g_shot_mode = 0;
static int  g_shot_pair = 0;
static int  g_shot_kb = 0;
static int  g_shot_err = 0;
static int  g_shot_cap = 0;
static int  g_shot_errbar = 0;
static int  g_shot_w = 1120;
static int  g_shot_h = 760;

static void shot_prepare(App *a)
{
    if (g_shot_cred) {
        /* 截图：把启动流程退回到「还没填凭证」的首次启动状态（只动标记，不删数据）。 */
        aic_setting_set(a->db, AIC_SET_CRED_HAS_KEY, "0");
        aic_router_boot(a->router);
        aic_router_config_check(a->router);
        return;
    }
    if (g_shot_mode) {
        /* 截图：凭证就绪但还没选默认模式。 */
        aic_setting_set(a->db, AIC_SET_CRED_HAS_KEY, "1");
        aic_setting_set_int(a->db, AIC_SET_FIRST_RUN_DONE, 0);
        aic_router_boot(a->router);
        aic_router_config_check(a->router);
        return;
    }
    if (g_shot_ready) {
        aic_route_state st = aic_router_state(a->router);
        if (st == AIC_ST_WIZARD) {
            aic_router_finish_wizard(a->router);
            app_setup_engine(a);
            app_reload_threads(a);
            app_load_chat(a);
        } else if (st == AIC_ST_CREDENTIAL_CHECK || st == AIC_ST_CREDENTIAL_SETUP) {
            aic_router_credential_done(a->router);
            st = aic_router_state(a->router);
        }
        if (st == AIC_ST_MODE_SELECT) aic_router_mode_select(a->router, AIC_MODE_CHAT);
    }
    if (g_shot_qa) {
        aic_conversation *arr = NULL;
        size_t n = 0;
        aic_router_switch_mode(a->router, AIC_MODE_QA);
        aic_router_finish_switch(a->router);
        if (aic_conv_list(a->db, AIC_MODE_QA, &arr, &n) == AIC_OK && n == 0) {
            aic_conversation c;
            memset(&c, 0, sizeof(c));
            if (aic_conv_create(a->db, AIC_MODE_QA, "如何安排一天的学习计划？", NULL, AIC_MODEL_FLASH, &c) == AIC_OK) {
                aic_msg_append(a->db, c.id, AIC_ROLE_USER, "怎么安排一天的学习时间？", AIC_MODEL_FLASH, NULL);
                aic_msg_append(a->db, c.id, AIC_ROLE_ASSISTANT,
                               "可以先按 25 分钟一段来做：上午处理最需要动脑的科目，下午做题与复盘，晚上只做轻量整理。", AIC_MODEL_FLASH, NULL);
                aic_router_set_thread(a->router, c.id);
            }
            memset(&c, 0, sizeof(c));
            aic_conv_create(a->db, AIC_MODE_QA, "帮我写一段生日祝福", NULL, AIC_MODEL_PRO, &c);
            memset(&c, 0, sizeof(c));
            aic_conv_create(a->db, AIC_MODE_QA, "解释一下什么是递归", NULL, AIC_MODEL_FLASH, &c);
        }
        if (arr) aic_conv_free(arr, n);
        app_reload_threads(a);
        app_load_chat(a);
    }
    if (g_shot_demo && a->conv_id[0]) {
        long long cnt = 0;
        if (aic_msg_count(a->db, a->conv_id, &cnt) == AIC_OK && cnt == 0) {
            aic_msg_append(a->db, a->conv_id, AIC_ROLE_USER, "今天有点累，能陪我聊一会儿吗？", AIC_MODEL_FLASH, NULL);
            aic_msg_append(a->db, a->conv_id, AIC_ROLE_ASSISTANT,
                           "当然可以呀。今天发生什么了？要是想先歇一会儿，我们就慢慢说，不着急。", AIC_MODEL_FLASH, NULL);
            aic_msg_append(a->db, a->conv_id, AIC_ROLE_USER, "也没什么大事，就是事情堆在一起，有点喘不过气。", AIC_MODEL_FLASH, NULL);
            aic_msg_append(a->db, a->conv_id, AIC_ROLE_ASSISTANT,
                           "那我们先从最容易的一件开始吧——你告诉我最让你烦心的是哪件，我陪你一起拆一拆。", AIC_MODEL_FLASH, NULL);
        }
        app_load_chat(a);
    }
    if (g_shot_tab >= 0) aic_router_set_tab(a->router, g_shot_tab);
    if (g_shot_adv) { aic_router_set_tab(a->router, AIC_TAB_SETTINGS); a->page_advanced = 1; a->scroll[1] = 0; }
    if (g_shot_pair) {
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_pair = 1;
        a->scroll[1] = 0;
        app_sync_start(a);
    }
    if (g_shot_kb) {
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_advanced = 0;
        a->page_pair = 0;
        a->page_error = 0;
        a->page_cap = 0;
        a->page_kb = 1;
        a->kb_focus = 0;
        a->kb_article[0] = '\0';
        a->kb_para = -1;
        a->kb_hit_n = 0;
        a->scroll[1] = 0;
        app_kb_refresh(a);
        if (g_shot_kb == 2 && a->kb) {
            const aic_kb_article *a0 = aic_kb_article_at(a->kb, 0, 0);
            if (a0) {
                app_kb_open(a, a0->id);
                a->kb_para = 0;
            }
        }
    }
    if (g_shot_err) {
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_kb = 0;
        a->page_cap = 0;
        a->page_error = 1;
        a->scroll[1] = 0;
        app_err_reload(a);
        if (a->err_n == 0) {
            aic_error_record(a->db, a->root, AIC_ERR_NETWORK, "llm.network",
                             "网络不可达，请检查网络后重试", a->conv_id, NULL, "curl: 连接超时", NULL);
            aic_error_record(a->db, a->root, AIC_ERR_QUOTA, "llm.balance_low",
                             "账户余额不足，无法继续对话", a->conv_id, NULL, NULL, NULL);
            app_err_reload(a);
        }
    }
    if (g_shot_cap) {
        aic_router_set_tab(a->router, AIC_TAB_SETTINGS);
        a->page_kb = 0;
        a->page_error = 0;
        a->page_cap = 1;
        a->scroll[1] = 0;
        a->app_n = 0;
    }
    if (g_shot_errbar) {
        /* 聊天页顶部那条非侵入状态条：有未读错误时出现，点了进错误中心。 */
        aic_router_set_tab(a->router, AIC_TAB_CHAT);
        if (aic_error_unread(a->db) == 0) {
            aic_error_record(a->db, a->root, AIC_ERR_NETWORK, "llm.network",
                             "网络不可达，请检查网络后重试", a->conv_id, NULL, NULL, NULL);
        }
    }
    if (g_shot_guide) app_guide(a);
    if (g_shot_overlay >= 0 && !g_shot_guide) {
        if (g_shot_overlay == 0) app_msg_open(a, -1);
        else if (g_shot_overlay == 1) app_persona_edit(a);
        else if (g_shot_overlay == 2) app_set_overlay(a, AIC_OV_UPDATE, "检查更新");
        else if (g_shot_overlay == 3) {
            app_set_overlay(a, AIC_OV_CONFIRM, "导出聊天记录");
            a->confirm_action = FORM_EXPORT;
            snprintf(a->overlay_text, sizeof(a->overlay_text),
                     "导出到 %s\\exports。JSON 可再次导入（含 format/schema_version/checksum）；"
                     "Markdown 与纯文本仅供阅读，不参与导入。导出包不含凭证。", a->root);
        }
        else if (g_shot_overlay == 4) app_msg_open(a, 0);
        else if (g_shot_overlay == 5) app_data(a);
        else if (g_shot_overlay == 6) app_plugin_panel(a);
        else app_guide(a);
    }
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App *a = &g_app;

    switch (msg) {
    case WM_CREATE:
        a->hwnd = hwnd;
        app_init(a);
        SetTimer(hwnd, ANIM_TIMER_ID, ANIM_FRAME_MS, NULL);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HDC mem;
        HBITMAP bm;
        HGDIOBJ old;
        GetClientRect(hwnd, &rc);
        if (g_diag) { static int np = 0; if (np < 3) { np++; DLOG("[diag] paint %ldx%ld", (long)rc.right, (long)rc.bottom); } }
        mem = CreateCompatibleDC(hdc);
        bm = CreateCompatibleBitmap(hdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        old = SelectObject(mem, bm);
        paint(a, mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 900;
        mm->ptMinTrackSize.y = 620;
        return 0;
    }
    case WM_TIMER:
        if (wp == ANIM_TIMER_ID) {
            long long now = aic_now_ms();
            if (aic_setting_get_int(a->db, AIC_SET_UPDATE_AUTO, 1)) {
                long long last = (long long)aic_setting_get_int(a->db, "update.last_check_ms", 0);
                int hours = aic_setting_get_int(a->db, AIC_SET_UPDATE_INTERVAL, 24);
                if (aic_update_should_check(last, hours, now)) app_check_update(a, 0);
            }
            if (a->anim_on && now - a->anim_start >= a->anim_dur) a->anim_on = 0;
            if (a->anim_on || (now - a->caret_tick) > 250) {
                a->caret_tick = now;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_SETTINGCHANGE:
        a->system_night = read_system_night();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP_UPD:
        app_on_upd(a, (TextMsg *)lp);
        return 0;
    case WM_APP_BAL:
        app_on_bal(a, (TextMsg *)lp);
        return 0;
    case WM_APP_IO:
        app_on_io(a, (IoMsg *)lp);
        return 0;
    case WM_APP_DELTA: {
        char *d = (char *)lp;
        if (d) {
            size_t left = sizeof(a->stream_text) - a->stream_len - 1;
            size_t dl = strlen(d);
            if (dl > left) dl = left;
            if (dl > 0) {
                memcpy(a->stream_text + a->stream_len, d, dl);
                a->stream_len += dl;
                a->stream_text[a->stream_len] = '\0';
            }
            aic_xfree(d);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_APP_DONE:
        app_on_done(a, (ResultMsg *)lp);
        return 0;
    case WM_APP_SYNC:
        /* 增量事件搬运完成：对端事件可能改了本地会话，重新载一次。 */
        app_load_chat(a);
        app_status(a, "同步：发送 %d 条、接收 %d 条", (int)wp, (int)lp);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP_PAIR:
        InterlockedExchange(&a->pair_busy, 0);
        a->pair_focus = 0;
        if ((int)wp == AIC_OK && a->ses && aic_session_is_paired(a->ses)) {
            app_load_chat(a);
            app_reload_threads(a);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        on_click(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_CHAR:
        on_char(a, (wchar_t)wp);
        return 0;
    case WM_KEYDOWN:
        on_key(a, (int)wp);
        return 0;
    case WM_MOUSEWHEEL:
        on_wheel(a, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd, ANIM_TIMER_ID);
        app_shutdown(a);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;

    (void)prev;
    (void)cmdline;
    {
        int argc = 0, i;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (i = 1; i < argc; i++) {
                char opt[64];
                w_to_u8(argv[i], opt, (int)sizeof(opt));
                if (!strcmp(opt, "--diag")) g_diag = 1;
                else if (!strcmp(opt, "--demo")) g_shot_demo = 1;
                else if (!strcmp(opt, "--qa")) g_shot_qa = 1;
                else if (!strcmp(opt, "--guide")) g_shot_guide = 1;
                else if (!strcmp(opt, "--adv")) { g_shot_adv = 1; g_shot_ready = 1; }
                else if (!strcmp(opt, "--ready")) g_shot_ready = 1;
                else if (!strcmp(opt, "--cred")) g_shot_cred = 1;
                else if (!strcmp(opt, "--mode")) g_shot_mode = 1;
                else if (!strcmp(opt, "--pair")) { g_shot_pair = 1; g_shot_ready = 1; }
                else if (!strcmp(opt, "--kb")) { g_shot_kb = 1; g_shot_ready = 1; }
                else if (!strcmp(opt, "--kbopen")) { g_shot_kb = 2; g_shot_ready = 1; }
                else if (!strcmp(opt, "--err")) { g_shot_err = 1; g_shot_ready = 1; }
                else if (!strcmp(opt, "--cap")) { g_shot_cap = 1; g_shot_ready = 1; }
                else if (!strcmp(opt, "--errbar")) { g_shot_errbar = 1; g_shot_ready = 1; g_shot_demo = 1; }
                else if (!strcmp(opt, "--shot") && i + 1 < argc) {
                    i++;
                    w_to_u8(argv[i], g_shot_path, (int)sizeof(g_shot_path));
                } else if (!strcmp(opt, "--tab") && i + 1 < argc) {
                    char v[16];
                    i++;
                    w_to_u8(argv[i], v, (int)sizeof(v));
                    g_shot_tab = atoi(v);
                } else if (!strcmp(opt, "--overlay") && i + 1 < argc) {
                    char v[16];
                    i++;
                    w_to_u8(argv[i], v, (int)sizeof(v));
                    g_shot_overlay = atoi(v);
                } else if (!strcmp(opt, "--size") && i + 1 < argc) {
                    char v[32];
                    i++;
                    w_to_u8(argv[i], v, (int)sizeof(v));
                    sscanf(v, "%dx%d", &g_shot_w, &g_shot_h);
                }
            }
            LocalFree(argv);
        }
    }
    DLOG("[diag] start diag=%d shot=%s", g_diag, g_shot_path);
    SetProcessDPIAware();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) { DLOG("[diag] RegisterClass failed err=%lu", GetLastError()); return 1; }

    g_app.hinst = hinst;
    hwnd = CreateWindowExW(0, APP_CLASS, L"Sai", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1120, 760,
                           NULL, NULL, hinst, NULL);
    if (!hwnd) { DLOG("[diag] CreateWindow failed err=%lu", GetLastError()); return 1; }
    DLOG("[diag] window created");

    ShowWindow(hwnd, show ? show : SW_SHOW);
    UpdateWindow(hwnd);
    DLOG("[diag] first paint done");

    if (g_shot_path[0]) {
        int rc;
        shot_prepare(&g_app);
        rc = app_shoot(&g_app, g_shot_w, g_shot_h, g_shot_path);
        DLOG("[diag] shot rc=%d path=%s", rc, g_shot_path);
    }

    if (g_diag || g_shot_path[0]) {
        DestroyWindow(hwnd);
        DLOG("[diag] ok");
        return 0;
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
