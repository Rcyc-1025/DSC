/* router.c —— 路由状态机实现。 */
#include "router.h"
#include "credential.h"
#include "store_setting.h"

#include <stdio.h>
#include <string.h>

struct aic_router {
    aic_db *db;
    aic_route_state state;
    aic_mode  mode;
    aic_tab   tab;
    int       switching;
    int       badges[AIC_TAB_COUNT];
    aic_route_state stack[AIC_ROUTER_STACK_MAX];
    int       stack_n;
    aic_overlay_kind overlays[AIC_ROUTER_OVERLAY_MAX];
    int       overlay_n;
    char      thread_id[40];
};

int aic_router_create(aic_db *db, aic_router **out)
{
    if (!db || !out) return AIC_E_INVAL;
    aic_router *r = (aic_router *)aic_xcalloc(1, sizeof(aic_router));
    r->db = db;
    r->state = AIC_ST_BOOT;
    r->mode = AIC_MODE_CHAT;
    r->tab = AIC_TAB_CHAT; /* 默认进聊天栏 */
    *out = r;
    return AIC_OK;
}

void aic_router_destroy(aic_router *r)
{
    aic_xfree(r);
}

aic_route_state aic_router_state(const aic_router *r) { return r ? r->state : AIC_ST_BOOT; }
aic_mode aic_router_mode(const aic_router *r) { return r ? r->mode : AIC_MODE_CHAT; }
aic_tab aic_router_tab(const aic_router *r) { return r ? r->tab : AIC_TAB_CHAT; }
int aic_router_is_switching(const aic_router *r) { return r ? r->switching : 0; }

const char *aic_route_state_name(aic_route_state s)
{
    switch (s) {
    case AIC_ST_BOOT:         return "BOOT";
    case AIC_ST_CONFIG_CHECK: return "CONFIG_CHECK";
    case AIC_ST_FIRST_RUN:    return "FIRST_RUN";
    case AIC_ST_WIZARD:       return "WIZARD";
    case AIC_ST_CHAT_READY:   return "CHAT_READY";
    case AIC_ST_QA_READY:     return "QA_READY";
    case AIC_ST_CHAT_TAB:     return "CHAT_TAB";
    case AIC_ST_QA_LIST:      return "QA_THREAD_LIST";
    case AIC_ST_QA_THREAD:    return "QA_THREAD";
    case AIC_ST_CREDENTIAL_CHECK: return "CREDENTIAL_CHECK";
    case AIC_ST_CREDENTIAL_SETUP: return "CREDENTIAL_SETUP";
    case AIC_ST_MODE_SELECT:      return "MODE_SELECT";
    default:                  return "UNKNOWN";
    }
}

const char *aic_tab_name(aic_tab t)
{
    switch (t) {
    case AIC_TAB_ME:       return "我";
    case AIC_TAB_SETTINGS: return "设置";
    case AIC_TAB_CHAT:     return "聊天";
    default:               return "未知";
    }
}

const char *aic_overlay_name(aic_overlay_kind k)
{
    switch (k) {
    case AIC_OV_NONE:     return "无";
    case AIC_OV_AI_INFO:  return "AI 信息";
    case AIC_OV_UPDATE:   return "更新提示";
    case AIC_OV_GUIDE:    return "新手指南";
    case AIC_OV_CONFIRM:  return "确认框";
    default:              return "未知";
    }
}

int aic_router_boot(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    r->state = AIC_ST_CONFIG_CHECK;
    return AIC_OK;
}

/* 凭证是否就绪。
 *
 * 以设置里的 credential.has_api_key 标记为准：API Key 保存成功后由界面层写入，
 * 保证「凭证写进安全存储之后才放行主页」（FR-30）。界面启动时会做一次补写，
 * 把安全存储里已存在但标记缺失的旧数据补齐。 */
static int cred_ready(aic_db *db)
{
    char v[16];
    v[0] = '\0';
    if (aic_setting_get(db, AIC_SET_CRED_HAS_KEY, v, sizeof(v)) == AIC_OK && strcmp(v, "1") == 0)
        return 1;
    return 0;
}

int aic_router_config_check(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->state != AIC_ST_CONFIG_CHECK) return AIC_E_STATE;
    r->state = AIC_ST_CREDENTIAL_CHECK;   /* FR-30：首次启动先检查凭证 */
    return aic_router_credential_check(r);
}

int aic_router_credential_check(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->state != AIC_ST_CREDENTIAL_CHECK) return AIC_E_STATE;

    if (!cred_ready(r->db)) {
        /* 凭证未就绪：必须先走凭证配置页，不能先进主页模式选择。 */
        r->state = AIC_ST_CREDENTIAL_SETUP;
        return AIC_OK;
    }
    char v[32];
    v[0] = '\0';
    int done = 0;
    if (aic_setting_get(r->db, AIC_SET_FIRST_RUN_DONE, v, sizeof(v)) == AIC_OK)
        done = (strcmp(v, "1") == 0);
    if (!done) {
        r->state = AIC_ST_MODE_SELECT;    /* 凭证就绪后先选默认模式 */
        return AIC_OK;
    }
    v[0] = '\0';
    aic_setting_get(r->db, AIC_SET_DEFAULT_MODE, v, sizeof(v));
    if (strcmp(v, "qa") == 0) {
        r->mode = AIC_MODE_QA;
        r->state = AIC_ST_QA_READY;
    } else {
        r->mode = AIC_MODE_CHAT;
        r->state = AIC_ST_CHAT_READY;
    }
    return AIC_OK;
}

int aic_router_credential_done(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->state != AIC_ST_CREDENTIAL_SETUP) return AIC_E_STATE;
    /* 凭证已写入安全存储（调用方负责），这里只放行到模式选择。 */
    r->state = AIC_ST_MODE_SELECT;
    return AIC_OK;
}

int aic_router_mode_select(aic_router *r, aic_mode mode)
{
    if (!r) return AIC_E_INVAL;
    if (r->state != AIC_ST_MODE_SELECT && r->state != AIC_ST_FIRST_RUN) return AIC_E_STATE;
    if (mode != AIC_MODE_CHAT && mode != AIC_MODE_QA) return AIC_E_INVAL;
    aic_setting_set(r->db, AIC_SET_DEFAULT_MODE, mode == AIC_MODE_QA ? "qa" : "chat");
    aic_setting_set(r->db, AIC_SET_FIRST_RUN_DONE, "1");
    r->mode = mode;
    r->state = (mode == AIC_MODE_CHAT) ? AIC_ST_CHAT_READY : AIC_ST_QA_READY;
    return AIC_OK;
}

int aic_router_start_wizard(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->state != AIC_ST_FIRST_RUN) return AIC_E_STATE;
    r->state = AIC_ST_WIZARD;
    return AIC_OK;
}

int aic_router_finish_wizard(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->state != AIC_ST_WIZARD) return AIC_E_STATE;
    aic_setting_set(r->db, AIC_SET_FIRST_RUN_DONE, "1");
    r->mode = AIC_MODE_CHAT;
    r->state = AIC_ST_CHAT_READY;
    return AIC_OK;
}

int aic_router_switch_mode(aic_router *r, aic_mode target)
{
    if (!r) return AIC_E_INVAL;
    if (r->switching) return AIC_E_BUSY;              /* 重复请求直接丢弃 */
    if (target != AIC_MODE_CHAT && target != AIC_MODE_QA) return AIC_E_INVAL;
    if ((r->state != AIC_ST_CHAT_READY && r->state != AIC_ST_CHAT_TAB &&
         r->state != AIC_ST_QA_READY && r->state != AIC_ST_QA_LIST &&
         r->state != AIC_ST_QA_THREAD)) {
        return AIC_E_STATE;
    }
    if (r->mode == target) return AIC_E_STATE;         /* 已经在目标模式 */
    r->switching = 1;
    r->mode = target;
    r->stack_n = 0;        /* 切换模式清空子页栈，但保留各栏状态 */
    r->overlay_n = 0;
    r->state = (target == AIC_MODE_CHAT) ? AIC_ST_CHAT_READY : AIC_ST_QA_READY;
    return AIC_OK;
}

int aic_router_finish_switch(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    r->switching = 0;
    return AIC_OK;
}

int aic_router_set_tab(aic_router *r, aic_tab tab)
{
    if (!r) return AIC_E_INVAL;
    if ((int)tab < 0 || (int)tab >= AIC_TAB_COUNT) return AIC_E_INVAL;
    if (r->mode != AIC_MODE_CHAT) return AIC_E_STATE;
    if (r->state != AIC_ST_CHAT_READY && r->state != AIC_ST_CHAT_TAB) return AIC_E_STATE;
    r->tab = tab;
    r->state = AIC_ST_CHAT_TAB;
    r->stack_n = 0;   /* 三栏同级，不入栈 */
    r->badges[tab] = 0;
    return AIC_OK;
}

int aic_router_set_badge(aic_router *r, aic_tab tab, int count)
{
    if (!r || (int)tab < 0 || (int)tab >= AIC_TAB_COUNT) return AIC_E_INVAL;
    r->badges[tab] = count > 0 ? count : 0;
    return AIC_OK;
}

int aic_router_badge(const aic_router *r, aic_tab tab)
{
    if (!r || (int)tab < 0 || (int)tab >= AIC_TAB_COUNT) return 0;
    return r->badges[tab];
}

int aic_router_push(aic_router *r, aic_route_state s)
{
    if (!r) return AIC_E_INVAL;
    if (r->stack_n >= AIC_ROUTER_STACK_MAX) return AIC_E_LIMIT;
    r->stack[r->stack_n++] = r->state;
    r->state = s;
    return AIC_OK;
}

int aic_router_pop(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->stack_n <= 0) return AIC_E_NOTFOUND;
    r->state = r->stack[--r->stack_n];
    return AIC_OK;
}

int aic_router_stack_depth(const aic_router *r) { return r ? r->stack_n : 0; }

int aic_router_open_overlay(aic_router *r, aic_overlay_kind k)
{
    if (!r) return AIC_E_INVAL;
    if (k == AIC_OV_NONE) return AIC_E_INVAL;
    if (r->overlay_n >= AIC_ROUTER_OVERLAY_MAX) return AIC_E_LIMIT;
    r->overlays[r->overlay_n++] = k;
    return AIC_OK;
}

int aic_router_close_overlay(aic_router *r)
{
    if (!r) return AIC_E_INVAL;
    if (r->overlay_n <= 0) return AIC_E_NOTFOUND;
    r->overlay_n--;
    return AIC_OK;
}

aic_overlay_kind aic_router_top_overlay(const aic_router *r)
{
    if (!r || r->overlay_n <= 0) return AIC_OV_NONE;
    return r->overlays[r->overlay_n - 1];
}

int aic_router_overlay_depth(const aic_router *r) { return r ? r->overlay_n : 0; }

aic_back_action aic_router_back(aic_router *r)
{
    if (!r) return AIC_BACK_NONE;
    if (r->overlay_n > 0) {
        r->overlay_n--;
        return AIC_BACK_CLOSED_OVERLAY;
    }
    if (r->stack_n > 0) {
        r->state = r->stack[--r->stack_n];
        return AIC_BACK_POPPED_SUBPAGE;
    }
    return AIC_BACK_EXIT_CONFIRM;
}

int aic_router_set_thread(aic_router *r, const char *conv_id)
{
    if (!r) return AIC_E_INVAL;
    if (r->mode != AIC_MODE_QA) return AIC_E_STATE;
    aic_strlcpy_utf8(r->thread_id, sizeof(r->thread_id), conv_id ? conv_id : "");
    r->state = conv_id && conv_id[0] ? AIC_ST_QA_THREAD : AIC_ST_QA_LIST;
    return AIC_OK;
}

const char *aic_router_thread(const aic_router *r)
{
    if (!r) return "";
    return r->thread_id;
}
