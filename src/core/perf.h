/* perf.h —— 性能预算与基准（FR-24）。
 *
 * 目标值来自技术设计 §13.4：桌面 / 移动两套预算，连续两版超出任一目标即阻断发布；
 * 劣化超过 10% 视为回归。本模块只负责「量」和「判」，不做界面。 */
#ifndef AIC_PERF_H
#define AIC_PERF_H

#include <stddef.h>

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aic_platform {
    AIC_PLATFORM_DESKTOP = 0,   /* Windows */
    AIC_PLATFORM_MOBILE = 1     /* Android */
} aic_platform;

typedef enum aic_perf_metric {
    AIC_PERF_COLD_START = 0,    /* 冷启动到可交互（毫秒） */
    AIC_PERF_MEMORY_IDLE,       /* 稳定态常驻内存（MB） */
    AIC_PERF_MODE_SWITCH,       /* 模式切换（毫秒） */
    AIC_PERF_TAB_SWITCH,        /* 三栏切换（毫秒） */
    AIC_PERF_FIRST_TOKEN,       /* 首段回复（毫秒） */
    AIC_PERF_PAGE_10K,          /* 万条消息分页（毫秒/页） */
    AIC_PERF_EXPORT_10K,        /* 万条消息导出（毫秒） */
    AIC_PERF_METRIC_COUNT
} aic_perf_metric;

/* 预算是「不超过」的阈值：毫秒或 MB。 */
int    aic_perf_budget(aic_platform platform, aic_perf_metric metric, double *out);
double aic_perf_budget_value(aic_platform platform, aic_perf_metric metric);
/* 1 表示达标；out_over 是超出的百分比（正数表示超标）。 */
int    aic_perf_check(aic_platform platform, aic_perf_metric metric, double value, double *out_over);
const char *aic_perf_metric_name(aic_perf_metric m);
const char *aic_perf_metric_unit(aic_perf_metric m);

/* 计时与内存采样。 */
long long aic_perf_now_us(void);
double    aic_perf_now_ms(void);
/* 当前进程常驻内存（KB），失败返回 AIC_E_UNSUPPORTED。 */
int       aic_perf_rss_kb(long long *out_kb);

/* 基准：分页（返回平均每页毫秒）与导出（返回总毫秒）。 */
int aic_perf_bench_page(aic_db *db, const char *conv_id, int pages, int page_size, double *out_ms);
int aic_perf_bench_export(aic_db *db, int mode, double *out_ms);

/* 生成一份人类可读的基准报告（用于自动化验收输出）。 */
int aic_perf_report(aic_platform platform, sb *out);

#ifdef __cplusplus
}
#endif

#endif /* AIC_PERF_H */
