/* store_guide.h —— 新手指南进度（FR-22 / 第 11 章）。 */
#ifndef AIC_STORE_GUIDE_H
#define AIC_STORE_GUIDE_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_GUIDE_STEP_COUNT 9

/* 步骤顺序按 §12.2：API Key 配置排在第一位（首次启动、进入主页之前），
 * 模式介绍在凭证就绪后。guide_progress 以 step_id 为键，旧版本已有的完成记录
 * 不会因为新增步骤或调整顺序而丢失（§12.1）。 */

typedef struct aic_guide_step {
    const char *id;        /* 稳定标识，写入 guide_progress */
    const char *title;     /* 步骤标题 */
    const char *trigger;   /* 触发条件 */
    const char *goal;      /* 目标 */
    const char *done_when; /* 完成条件 */
    const char *group;     /* 引入该步骤的版本号（§12.1 按版本分组） */
} aic_guide_step;

typedef struct aic_guide_state {
    int completed[AIC_GUIDE_STEP_COUNT];
    int skipped[AIC_GUIDE_STEP_COUNT];
    int completed_count;
    int next_index;        /* 下一个未完成步骤下标，全部完成时等于步数 */
} aic_guide_state;

const aic_guide_step *aic_guide_steps(void);
int  aic_guide_step_index(const char *step_id);   /* 未找到返回 -1 */
int  aic_guide_mark(aic_db *db, const char *step_id, int skipped);
int  aic_guide_reset(aic_db *db);
int  aic_guide_state_get(aic_db *db, aic_guide_state *out);

#ifdef __cplusplus
}
#endif

#endif
