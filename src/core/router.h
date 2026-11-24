/* router.h —— 路由状态机（FR-13 双模式、FR-15 三栏、页面栈与浮层）。
 *
 * 状态迁移（技术设计 §4.4 / §2.3，FR-30 首次启动顺序）：
 *   BOOT -> CONFIG_CHECK -> CREDENTIAL_CHECK
 *   CREDENTIAL_CHECK -> 无凭证 CREDENTIAL_SETUP ｜ 有凭证 MODE_SELECT
 *   CREDENTIAL_SETUP -> MODE_SELECT       （凭证写入安全存储之后才放行主页）
 *   MODE_SELECT -> { CHAT_READY | QA_READY }（首次记录 default_mode）
 *   CREDENTIAL_CHECK -> 已记录 default_mode 时直达 { CHAT_READY | QA_READY }
 *   保留旧路径 FIRST_RUN -> WIZARD -> CHAT_READY（老版本数据兼容）
 *   CHAT_READY <-> QA_READY      （mode_switch）
 *   CHAT_READY -> CHAT_TAB(我|设置|聊天)
 *   QA_READY   -> QA_THREAD_LIST | QA_THREAD
 *   任意状态   -> OVERLAY(AI_INFO|UPDATE_DIALOG|GUIDE|CONFIRM)（不销毁底层状态）
 *
 * 约束：同一时刻只有一个模式活动；mode_switch 期间置 switching 标志，重复请求直接丢弃；
 * 三栏切换是同级的，不进入页面栈；AI 信息、设置子页、引导步骤才入栈。 */
#ifndef DSC_ROUTER_H
#define DSC_ROUTER_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dsc_route_state {
    DSC_ST_BOOT = 0,
    DSC_ST_CONFIG_CHECK,
    DSC_ST_FIRST_RUN,
    DSC_ST_WIZARD,
    DSC_ST_CHAT_READY,
    DSC_ST_QA_READY,
    DSC_ST_CHAT_TAB,
    DSC_ST_QA_LIST,
    DSC_ST_QA_THREAD,
    DSC_ST_CREDENTIAL_CHECK,   /* 首次启动第一步：检查凭证是否就绪 */
    DSC_ST_CREDENTIAL_SETUP,   /* 凭证未就绪：必须先配置 API Key */
    DSC_ST_MODE_SELECT         /* 凭证就绪：选择默认模式 */
} dsc_route_state;

typedef enum dsc_tab {
    DSC_TAB_ME = 0,
    DSC_TAB_SETTINGS = 1,
    DSC_TAB_CHAT = 2,
    DSC_TAB_COUNT
} dsc_tab;

typedef enum dsc_overlay_kind {
    DSC_OV_NONE = 0,
    DSC_OV_AI_INFO,
    DSC_OV_UPDATE,
    DSC_OV_GUIDE,
    DSC_OV_CONFIRM
} dsc_overlay_kind;

typedef enum dsc_back_action {
    DSC_BACK_NONE = 0,
    DSC_BACK_CLOSED_OVERLAY,
    DSC_BACK_POPPED_SUBPAGE,
    DSC_BACK_EXIT_CONFIRM
} dsc_back_action;

#define DSC_ROUTER_STACK_MAX   16
#define DSC_ROUTER_OVERLAY_MAX 8

typedef struct dsc_router dsc_router;

int  dsc_router_create(dsc_db *db, dsc_router **out);
void dsc_router_destroy(dsc_router *r);

dsc_route_state dsc_router_state(const dsc_router *r);
dsc_mode        dsc_router_mode(const dsc_router *r);
dsc_tab         dsc_router_tab(const dsc_router *r);
int             dsc_router_is_switching(const dsc_router *r);
const char     *dsc_route_state_name(dsc_route_state s);
const char     *dsc_tab_name(dsc_tab t);
const char     *dsc_overlay_name(dsc_overlay_kind k);

/* BOOT -> CONFIG_CHECK：读设置决定首次运行还是进入默认模式。 */
int dsc_router_boot(dsc_router *r);
/* CONFIG_CHECK -> CREDENTIAL_CHECK ->（凭证就绪）MODE_SELECT 或直达目标模式。 */
int dsc_router_config_check(dsc_router *r);
/* CREDENTIAL_CHECK：无凭证进 CREDENTIAL_SETUP，有凭证按已记录的默认模式分流。 */
int dsc_router_credential_check(dsc_router *r);
/* CREDENTIAL_SETUP -> MODE_SELECT：凭证已成功写入安全存储后调用。 */
int dsc_router_credential_done(dsc_router *r);
/* MODE_SELECT -> CHAT_READY | QA_READY：记录 default_mode 并放行主页。 */
int dsc_router_mode_select(dsc_router *r, dsc_mode mode);
/* FIRST_RUN -> WIZARD，WIZARD -> CHAT_READY。 */
int dsc_router_start_wizard(dsc_router *r);
int dsc_router_finish_wizard(dsc_router *r);

/* 模式切换：switching 期间重复请求返回 DSC_E_BUSY 并被丢弃。 */
int dsc_router_switch_mode(dsc_router *r, dsc_mode target);
/* 动画结束时调用，清 switching 标志。 */
int dsc_router_finish_switch(dsc_router *r);

/* 三栏切换（仅聊天模式就绪；同级路由不入栈）。 */
int dsc_router_set_tab(dsc_router *r, dsc_tab tab);
/* 角标：其他栏有新消息时显示数字。 */
int  dsc_router_set_badge(dsc_router *r, dsc_tab tab, int count);
int  dsc_router_badge(const dsc_router *r, dsc_tab tab);

/* 子页入栈/出栈（AI 信息、设置子页）。 */
int dsc_router_push(dsc_router *r, dsc_route_state s);
int dsc_router_pop(dsc_router *r);
int dsc_router_stack_depth(const dsc_router *r);

/* 浮层（不销毁底层状态）。 */
int dsc_router_open_overlay(dsc_router *r, dsc_overlay_kind k);
int dsc_router_close_overlay(dsc_router *r);
dsc_overlay_kind dsc_router_top_overlay(const dsc_router *r);
int dsc_router_overlay_depth(const dsc_router *r);

/* 返回键：优先关浮层 -> 上一子页 -> 退出确认。 */
dsc_back_action dsc_router_back(dsc_router *r);

/* 问答模式的会话选择（FR-20）：切换会话只换当前线程，不动其他状态。 */
int dsc_router_set_thread(dsc_router *r, const char *conv_id);
const char *dsc_router_thread(const dsc_router *r);

#ifdef __cplusplus
}
#endif

#endif /* DSC_ROUTER_H */
