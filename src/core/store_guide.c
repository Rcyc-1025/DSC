/* store_guide.c */
#include "store_guide.h"

#include <string.h>

static const aic_guide_step g_steps[AIC_GUIDE_STEP_COUNT] = {
    { "mode_intro", "模式介绍", "首次启动", "认识问答模式与聊天模式，选择默认模式", "在模式切换入口选择过一次默认模式" },
    { "api_key", "API Key 配置", "首次进入设置或首次发送消息", "配置可用的模型凭证", "Key 输入框提交且连通性校验通过" },
    { "first_chat", "首次对话", "凭证就绪后", "完成第一轮对话", "聊天输入区完成一轮往返" },
    { "persona_edit", "人设修改", "首轮对话后", "改写一次人设提示词", "在 AI 信息入口保存过一次" },
    { "profile_fill", "资料填写", "首次进入「我」", "填写并保存个人资料", "姓名输入框保存一次" },
    { "record_transfer", "记录导出与导入", "首次产生三条消息后", "各完成一次导出与导入", "设置页数据项里导出、导入各成功一次" },
    { "plugin_import", "插件导入", "首次进入插件面板", "安装一个插件", "插件导入入口成功安装一次" },
    { "balance_query", "余额查询", "首次产生费用后", "查看一次余额与用量", "余额入口查询成功一次" },
    { "update_guard", "更新与数据保护", "首次更新前", "了解更新前的备份规则", "在更新设置里确认过一次备份说明" },
};

const aic_guide_step *aic_guide_steps(void)
{
    return g_steps;
}

int aic_guide_step_index(const char *step_id)
{
    if (!step_id) return -1;
    for (int i = 0; i < AIC_GUIDE_STEP_COUNT; i++)
        if (strcmp(g_steps[i].id, step_id) == 0) return i;
    return -1;
}

int aic_guide_mark(aic_db *db, const char *step_id, int skipped)
{
    if (!db || !step_id) return AIC_E_INVAL;
    if (aic_guide_step_index(step_id) < 0) return AIC_E_INVAL;
    char now[32];
    aic_now_iso8601(now);
    aic_stmt *st = aic_db_prepare(db,
        "INSERT INTO guide_progress (step_id, completed_at, skipped) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(step_id) DO UPDATE SET completed_at = excluded.completed_at, skipped = excluded.skipped;");
    if (!st) return AIC_E_DB;
    aic_stmt_bind_text(st, 1, step_id);
    aic_stmt_bind_text(st, 2, now);
    aic_stmt_bind_ll(st, 3, skipped ? 1 : 0);
    int rc = aic_stmt_step(st);
    aic_stmt_finalize(st);
    return rc == AIC_DONE ? AIC_OK : rc;
}

int aic_guide_reset(aic_db *db)
{
    if (!db) return AIC_E_INVAL;
    return aic_db_exec(db, "DELETE FROM guide_progress;");
}

int aic_guide_state_get(aic_db *db, aic_guide_state *out)
{
    if (!db || !out) return AIC_E_INVAL;
    memset(out, 0, sizeof(*out));
    aic_stmt *st = aic_db_prepare(db, "SELECT step_id, skipped FROM guide_progress;");
    if (!st) return AIC_E_DB;
    int rc;
    while ((rc = aic_stmt_step(st)) == AIC_ROW) {
        const char *id = aic_stmt_text(st, 0);
        int idx = aic_guide_step_index(id);
        if (idx >= 0) {
            out->completed[idx] = 1;
            out->skipped[idx] = aic_stmt_int(st, 1);
        }
    }
    aic_stmt_finalize(st);
    if (rc != AIC_DONE) return rc;
    out->completed_count = 0;
    out->next_index = -1;
    for (int i = 0; i < AIC_GUIDE_STEP_COUNT; i++) {
        if (out->completed[i] && !out->skipped[i]) out->completed_count++;
        else if (out->next_index < 0) out->next_index = i;
    }
    if (out->next_index < 0) out->next_index = AIC_GUIDE_STEP_COUNT;
    return AIC_OK;
}
