/* store_guide.c */
#include "store_guide.h"

#include <string.h>

static const dsc_guide_step g_steps[DSC_GUIDE_STEP_COUNT] = {
    /* §12.2：API Key 配置是首次启动的第一步，必须先于进入主页模式选择完成。 */
    { "api_key", "API Key 配置", "首次启动、进入主页之前", "配置可用的模型凭证", "Key 输入框提交且凭证写入安全存储并通过格式校验", "1.0" },
    { "mode_intro", "模式介绍", "凭证就绪后首次进入主页", "认识问答模式与聊天模式并选定默认模式", "在模式选择处选定过一次默认模式", "1.0" },
    { "first_chat", "首次对话", "凭证就绪且已选定模式", "完成第一轮对话", "聊天输入区完成一轮往返", "1.0" },
    { "persona_edit", "人设修改", "首轮对话后", "改写一次人设提示词", "在 AI 信息入口保存过一次", "1.0" },
    { "profile_fill", "资料填写", "首次进入「我」", "填写并保存个人资料", "姓名输入框保存一次", "1.0" },
    { "record_transfer", "记录导出与导入", "首次产生三条消息后", "各完成一次导出与导入", "设置页数据项里导出、导入各成功一次", "1.0" },
    { "plugin_import", "插件导入", "首次进入插件面板", "安装一个插件", "插件导入入口成功安装一次", "1.0" },
    { "balance_query", "余额查询", "首次产生费用后", "查看一次余额与用量", "余额入口查询成功一次", "1.0" },
    { "update_guard", "更新与数据保护", "首次更新前", "了解更新前的备份规则", "在更新设置里确认过一次备份说明", "1.0" },
};

const dsc_guide_step *dsc_guide_steps(void)
{
    return g_steps;
}

int dsc_guide_step_index(const char *step_id)
{
    if (!step_id) return -1;
    for (int i = 0; i < DSC_GUIDE_STEP_COUNT; i++)
        if (strcmp(g_steps[i].id, step_id) == 0) return i;
    return -1;
}

int dsc_guide_mark(dsc_db *db, const char *step_id, int skipped)
{
    if (!db || !step_id) return DSC_E_INVAL;
    if (dsc_guide_step_index(step_id) < 0) return DSC_E_INVAL;
    char now[32];
    dsc_now_iso8601(now);
    dsc_stmt *st = dsc_db_prepare(db,
        "INSERT INTO guide_progress (step_id, completed_at, skipped) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(step_id) DO UPDATE SET completed_at = excluded.completed_at, skipped = excluded.skipped;");
    if (!st) return DSC_E_DB;
    dsc_stmt_bind_text(st, 1, step_id);
    dsc_stmt_bind_text(st, 2, now);
    dsc_stmt_bind_ll(st, 3, skipped ? 1 : 0);
    int rc = dsc_stmt_step(st);
    dsc_stmt_finalize(st);
    return rc == DSC_DONE ? DSC_OK : rc;
}

int dsc_guide_reset(dsc_db *db)
{
    if (!db) return DSC_E_INVAL;
    return dsc_db_exec(db, "DELETE FROM guide_progress;");
}

int dsc_guide_state_get(dsc_db *db, dsc_guide_state *out)
{
    if (!db || !out) return DSC_E_INVAL;
    memset(out, 0, sizeof(*out));
    dsc_stmt *st = dsc_db_prepare(db, "SELECT step_id, skipped FROM guide_progress;");
    if (!st) return DSC_E_DB;
    int rc;
    while ((rc = dsc_stmt_step(st)) == DSC_ROW) {
        const char *id = dsc_stmt_text(st, 0);
        int idx = dsc_guide_step_index(id);
        if (idx >= 0) {
            out->completed[idx] = 1;
            out->skipped[idx] = dsc_stmt_int(st, 1);
        }
    }
    dsc_stmt_finalize(st);
    if (rc != DSC_DONE) return rc;
    out->completed_count = 0;
    out->next_index = -1;
    for (int i = 0; i < DSC_GUIDE_STEP_COUNT; i++) {
        if (out->completed[i] && !out->skipped[i]) out->completed_count++;
        else if (out->next_index < 0) out->next_index = i;
    }
    if (out->next_index < 0) out->next_index = DSC_GUIDE_STEP_COUNT;
    return DSC_OK;
}
