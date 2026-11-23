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
#ifndef AIC_ROUTER_H
#define AIC_ROUTER_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aic_route_state {
    AIC_ST_BOOT = 0,
    AIC_ST_CONFIG_CHECK,
    AIC_ST_FIRST_RUN,
    AIC_ST_WIZARD,
    AIC_ST_CHAT_READY,
    AIC_ST_QA_READY,
    AIC_ST_CHAT_TAB,
    AIC_ST_QA_LIST,
    AIC_ST_QA_THREAD,
    AIC_ST_CREDENTIAL_CHECK,   /* 首次启动第一步：检查凭证是否就绪 */
    AIC_ST_CREDENTIAL_SETUP,   /* 凭证未就绪：必须先配置 API Key */
    AIC_ST_MODE_SELECT         /* 凭证就绪：选择默认模式 */
} aic_route_state;

typedef enum aic_tab {
    AIC_TAB_ME = 0,
    AIC_TAB_SETTINGS = 1,
    AIC_TAB_CHAT = 2,
    AIC_TAB_COUNT
} aic_tab;

typedef enum aic_overlay_kind {
    AIC_OV_NONE = 0,
    AIC_OV_AI_INFO,
    AIC_OV_UPDATE,
    AIC_OV_GUIDE,
    AIC_OV_CONFIRM
} aic_overlay_kind;

typedef enum aic_back_action {
    AIC_BACK_NONE = 0,
    AIC_BACK_CLOSED_OVERLAY,
    AIC_BACK_POPPED_SUBPAGE,
    AIC_BACK_EXIT_CONFIRM
} aic_back_action;

#define AIC_ROUTER_STACK_MAX   16
#define AIC_ROUTER_OVERLAY_MAX 8

typedef struct aic_router aic_router;

int  aic_router_create(aic_db *db, aic_router **out);
void aic_router_destroy(aic_router *r);

aic_route_state aic_router_state(const aic_router *r);
aic_mode        aic_router_mode(const aic_router *r);
aic_tab         aic_router_tab(const aic_router *r);
int             aic_router_is_switching(const aic_router *r);
const char     *aic_route_state_name(aic_route_state s);
const char     *aic_tab_name(aic_tab t);
const char     *aic_overlay_name(aic_overlay_kind k);

/* BOOT -> CONFIG_CHECK：读设置决定首次运行还是进入默认模式。 */
int aic_router_boot(aic_router *r);
/* CONFIG_CHECK -> CREDENTIAL_CHECK ->（凭证就绪）MODE_SELECT 或直达目标模式。 */
int aic_router_config_check(aic_router *r);
/* CREDENTIAL_CHECK：无凭证进 CREDENTIAL_SETUP，有凭证按已记录的默认模式分流。 */
int aic_router_credential_check(aic_router *r);
/* CREDENTIAL_SETUP -> MODE_SELECT：凭证已成功写入安全存储后调用。 */
int aic_router_credential_done(aic_router *r);
/* MODE_SELECT -> CHAT_READY | QA_READY：记录 default_mode 并放行主页。 */
int aic_router_mode_select(aic_router *r, aic_mode mode);
/* FIRST_RUN -> WIZARD，WIZARD -> CHAT_READY。 */
int aic_router_start_wizard(aic_router *r);
int aic_router_finish_wizard(aic_router *r);

/* 模式切换：switching 期间重复请求返回 AIC_E_BUSY 并被丢弃。 */
int aic_router_switch_mode(aic_router *r, aic_mode target);
/* 动画结束时调用，清 switching 标志。 */
int aic_router_finish_switch(aic_router *r);

/* 三栏切换（仅聊天模式就绪；同级路由不入栈）。 */
int aic_router_set_tab(aic_router *r, aic_tab tab);
/* 角标：其他栏有新消息时显示数字。 */
int  aic_router_set_badge(aic_router *r, aic_tab tab, int count);
int  aic_router_badge(const aic_router *r, aic_tab tab);

/* 子页入栈/出栈（AI 信息、设置子页）。 */
int aic_router_push(aic_router *r, aic_route_state s);
int aic_router_pop(aic_router *r);
int aic_router_stack_depth(const aic_router *r);

/* 浮层（不销毁底层状态）。 */
int aic_router_open_overlay(aic_router *r, aic_overlay_kind k);
int aic_router_close_overlay(aic_router *r);
aic_overlay_kind aic_router_top_overlay(const aic_router *r);
int aic_router_overlay_depth(const aic_router *r);

/* 返回键：优先关浮层 -> 上一子页 -> 退出确认。 */
aic_back_action aic_router_back(aic_router *r);

/* 问答模式的会话选择（FR-20）：切换会话只换当前线程，不动其他状态。 */
int aic_router_set_thread(aic_router *r, const char *conv_id);
const char *aic_router_thread(const aic_router *r);

#ifdef __cplusplus
}
#endif

#endif /* AIC_ROUTER_H */
