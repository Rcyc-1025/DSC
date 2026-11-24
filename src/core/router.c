/* router.c —— 路由状态机实现。 */
#include "router.h"
#include "credential.h"
#include "store_setting.h"

#include <stdio.h>
#include <string.h>

struct dsc_router {
    dsc_db *db;
    dsc_route_state state;
    dsc_mode  mode;
    dsc_tab   tab;
    int       switching;
    int       badges[DSC_TAB_COUNT];
    dsc_route_state stack[DSC_ROUTER_STACK_MAX];
    int       stack_n;
    dsc_overlay_kind overlays[DSC_ROUTER_OVERLAY_MAX];
    int       overlay_n;
    char      thread_id[40];
};

int dsc_router_create(dsc_db *db, dsc_router **out)
{
    if (!db || !out) return DSC_E_INVAL;
    dsc_router *r = (dsc_router *)dsc_xcalloc(1, sizeof(dsc_router));
    r->db = db;
    r->state = DSC_ST_BOOT;
    r->mode = DSC_MODE_CHAT;
    r->tab = DSC_TAB_CHAT; /* 默认进聊天栏 */
    *out = r;
    return DSC_OK;
}

void dsc_router_destroy(dsc_router *r)
{
    dsc_xfree(r);
}

dsc_route_state dsc_router_state(const dsc_router *r) { return r ? r->state : DSC_ST_BOOT; }
dsc_mode dsc_router_mode(const dsc_router *r) { return r ? r->mode : DSC_MODE_CHAT; }
dsc_tab dsc_router_tab(const dsc_router *r) { return r ? r->tab : DSC_TAB_CHAT; }
int dsc_router_is_switching(const dsc_router *r) { return r ? r->switching : 0; }

const char *dsc_route_state_name(dsc_route_state s)
{
    switch (s) {
    case DSC_ST_BOOT:         return "BOOT";
    case DSC_ST_CONFIG_CHECK: return "CONFIG_CHECK";
    case DSC_ST_FIRST_RUN:    return "FIRST_RUN";
    case DSC_ST_WIZARD:       return "WIZARD";
    case DSC_ST_CHAT_READY:   return "CHAT_READY";
    case DSC_ST_QA_READY:     return "QA_READY";
    case DSC_ST_CHAT_TAB:     return "CHAT_TAB";
    case DSC_ST_QA_LIST:      return "QA_THREAD_LIST";
    case DSC_ST_QA_THREAD:    return "QA_THREAD";
    case DSC_ST_CREDENTIAL_CHECK: return "CREDENTIAL_CHECK";
    case DSC_ST_CREDENTIAL_SETUP: return "CREDENTIAL_SETUP";
    case DSC_ST_MODE_SELECT:      return "MODE_SELECT";
    default:                  return "UNKNOWN";
    }
}

const char *dsc_tab_name(dsc_tab t)
{
    switch (t) {
    case DSC_TAB_ME:       return "我";
    case DSC_TAB_SETTINGS: return "设置";
    case DSC_TAB_CHAT:     return "聊天";
    default:               return "未知";
    }
}

const char *dsc_overlay_name(dsc_overlay_kind k)
{
    switch (k) {
    case DSC_OV_NONE:     return "无";
    case DSC_OV_AI_INFO:  return "AI 信息";
    case DSC_OV_UPDATE:   return "更新提示";
    case DSC_OV_GUIDE:    return "新手指南";
    case DSC_OV_CONFIRM:  return "确认框";
    default:              return "未知";
    }
}

int dsc_router_boot(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    r->state = DSC_ST_CONFIG_CHECK;
    return DSC_OK;
}

/* 凭证是否就绪。
 *
 * 以设置里的 credential.has_api_key 标记为准：API Key 保存成功后由界面层写入，
 * 保证「凭证写进安全存储之后才放行主页」（FR-30）。界面启动时会做一次补写，
 * 把安全存储里已存在但标记缺失的旧数据补齐。 */
static int cred_ready(dsc_db *db)
{
    char v[16];
    v[0] = '\0';
    if (dsc_setting_get(db, DSC_SET_CRED_HAS_KEY, v, sizeof(v)) == DSC_OK && strcmp(v, "1") == 0)
        return 1;
    return 0;
}

int dsc_router_config_check(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->state != DSC_ST_CONFIG_CHECK) return DSC_E_STATE;
    r->state = DSC_ST_CREDENTIAL_CHECK;   /* FR-30：首次启动先检查凭证 */
    return dsc_router_credential_check(r);
}

int dsc_router_credential_check(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->state != DSC_ST_CREDENTIAL_CHECK) return DSC_E_STATE;

    if (!cred_ready(r->db)) {
        /* 凭证未就绪：必须先走凭证配置页，不能先进主页模式选择。 */
        r->state = DSC_ST_CREDENTIAL_SETUP;
        return DSC_OK;
    }
    char v[32];
    v[0] = '\0';
    int done = 0;
    if (dsc_setting_get(r->db, DSC_SET_FIRST_RUN_DONE, v, sizeof(v)) == DSC_OK)
        done = (strcmp(v, "1") == 0);
    if (!done) {
        r->state = DSC_ST_MODE_SELECT;    /* 凭证就绪后先选默认模式 */
        return DSC_OK;
    }
    v[0] = '\0';
    dsc_setting_get(r->db, DSC_SET_DEFAULT_MODE, v, sizeof(v));
    if (strcmp(v, "qa") == 0) {
        r->mode = DSC_MODE_QA;
        r->state = DSC_ST_QA_READY;
    } else {
        r->mode = DSC_MODE_CHAT;
        r->state = DSC_ST_CHAT_READY;
    }
    return DSC_OK;
}

int dsc_router_credential_done(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->state != DSC_ST_CREDENTIAL_SETUP) return DSC_E_STATE;
    /* 凭证已写入安全存储（调用方负责），这里只放行到模式选择。 */
    r->state = DSC_ST_MODE_SELECT;
    return DSC_OK;
}

int dsc_router_mode_select(dsc_router *r, dsc_mode mode)
{
    if (!r) return DSC_E_INVAL;
    if (r->state != DSC_ST_MODE_SELECT && r->state != DSC_ST_FIRST_RUN) return DSC_E_STATE;
    if (mode != DSC_MODE_CHAT && mode != DSC_MODE_QA) return DSC_E_INVAL;
    dsc_setting_set(r->db, DSC_SET_DEFAULT_MODE, mode == DSC_MODE_QA ? "qa" : "chat");
    dsc_setting_set(r->db, DSC_SET_FIRST_RUN_DONE, "1");
    r->mode = mode;
    r->state = (mode == DSC_MODE_CHAT) ? DSC_ST_CHAT_READY : DSC_ST_QA_READY;
    return DSC_OK;
}

int dsc_router_start_wizard(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->state != DSC_ST_FIRST_RUN) return DSC_E_STATE;
    r->state = DSC_ST_WIZARD;
    return DSC_OK;
}

int dsc_router_finish_wizard(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->state != DSC_ST_WIZARD) return DSC_E_STATE;
    dsc_setting_set(r->db, DSC_SET_FIRST_RUN_DONE, "1");
    r->mode = DSC_MODE_CHAT;
    r->state = DSC_ST_CHAT_READY;
    return DSC_OK;
}

int dsc_router_switch_mode(dsc_router *r, dsc_mode target)
{
    if (!r) return DSC_E_INVAL;
    if (r->switching) return DSC_E_BUSY;              /* 重复请求直接丢弃 */
    if (target != DSC_MODE_CHAT && target != DSC_MODE_QA) return DSC_E_INVAL;
    if ((r->state != DSC_ST_CHAT_READY && r->state != DSC_ST_CHAT_TAB &&
         r->state != DSC_ST_QA_READY && r->state != DSC_ST_QA_LIST &&
         r->state != DSC_ST_QA_THREAD)) {
        return DSC_E_STATE;
    }
    if (r->mode == target) return DSC_E_STATE;         /* 已经在目标模式 */
    r->switching = 1;
    r->mode = target;
    r->stack_n = 0;        /* 切换模式清空子页栈，但保留各栏状态 */
    r->overlay_n = 0;
    r->state = (target == DSC_MODE_CHAT) ? DSC_ST_CHAT_READY : DSC_ST_QA_READY;
    return DSC_OK;
}

int dsc_router_finish_switch(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    r->switching = 0;
    return DSC_OK;
}

int dsc_router_set_tab(dsc_router *r, dsc_tab tab)
{
    if (!r) return DSC_E_INVAL;
    if ((int)tab < 0 || (int)tab >= DSC_TAB_COUNT) return DSC_E_INVAL;
    if (r->mode != DSC_MODE_CHAT) return DSC_E_STATE;
    if (r->state != DSC_ST_CHAT_READY && r->state != DSC_ST_CHAT_TAB) return DSC_E_STATE;
    r->tab = tab;
    r->state = DSC_ST_CHAT_TAB;
    r->stack_n = 0;   /* 三栏同级，不入栈 */
    r->badges[tab] = 0;
    return DSC_OK;
}

int dsc_router_set_badge(dsc_router *r, dsc_tab tab, int count)
{
    if (!r || (int)tab < 0 || (int)tab >= DSC_TAB_COUNT) return DSC_E_INVAL;
    r->badges[tab] = count > 0 ? count : 0;
    return DSC_OK;
}

int dsc_router_badge(const dsc_router *r, dsc_tab tab)
{
    if (!r || (int)tab < 0 || (int)tab >= DSC_TAB_COUNT) return 0;
    return r->badges[tab];
}

int dsc_router_push(dsc_router *r, dsc_route_state s)
{
    if (!r) return DSC_E_INVAL;
    if (r->stack_n >= DSC_ROUTER_STACK_MAX) return DSC_E_LIMIT;
    r->stack[r->stack_n++] = r->state;
    r->state = s;
    return DSC_OK;
}

int dsc_router_pop(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->stack_n <= 0) return DSC_E_NOTFOUND;
    r->state = r->stack[--r->stack_n];
    return DSC_OK;
}

int dsc_router_stack_depth(const dsc_router *r) { return r ? r->stack_n : 0; }

int dsc_router_open_overlay(dsc_router *r, dsc_overlay_kind k)
{
    if (!r) return DSC_E_INVAL;
    if (k == DSC_OV_NONE) return DSC_E_INVAL;
    if (r->overlay_n >= DSC_ROUTER_OVERLAY_MAX) return DSC_E_LIMIT;
    r->overlays[r->overlay_n++] = k;
    return DSC_OK;
}

int dsc_router_close_overlay(dsc_router *r)
{
    if (!r) return DSC_E_INVAL;
    if (r->overlay_n <= 0) return DSC_E_NOTFOUND;
    r->overlay_n--;
    return DSC_OK;
}

dsc_overlay_kind dsc_router_top_overlay(const dsc_router *r)
{
    if (!r || r->overlay_n <= 0) return DSC_OV_NONE;
    return r->overlays[r->overlay_n - 1];
}

int dsc_router_overlay_depth(const dsc_router *r) { return r ? r->overlay_n : 0; }

dsc_back_action dsc_router_back(dsc_router *r)
{
    if (!r) return DSC_BACK_NONE;
    if (r->overlay_n > 0) {
        r->overlay_n--;
        return DSC_BACK_CLOSED_OVERLAY;
    }
    if (r->stack_n > 0) {
        r->state = r->stack[--r->stack_n];
        return DSC_BACK_POPPED_SUBPAGE;
    }
    return DSC_BACK_EXIT_CONFIRM;
}

int dsc_router_set_thread(dsc_router *r, const char *conv_id)
{
    if (!r) return DSC_E_INVAL;
    if (r->mode != DSC_MODE_QA) return DSC_E_STATE;
    dsc_strlcpy_utf8(r->thread_id, sizeof(r->thread_id), conv_id ? conv_id : "");
    r->state = conv_id && conv_id[0] ? DSC_ST_QA_THREAD : DSC_ST_QA_LIST;
    return DSC_OK;
}

const char *dsc_router_thread(const dsc_router *r)
{
    if (!r) return "";
    return r->thread_id;
}
