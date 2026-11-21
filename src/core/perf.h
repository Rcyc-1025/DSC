/* perf.h —— 性能预算与基准（FR-24）。
 *
 * 目标值来自技术设计 §13.2：桌面 / 移动两套预算，连续两版超出任一目标即阻断发布；
 * 劣化超过 10% 视为回归。除「量」和「判」之外，还提供 PerfMonitor 的采样接口
 * （内存、单帧耗时、切换耗时、存储分项），供开发版采样面板与 aic.exe perf 使用。 */
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

/* ---------- 采样（PerfMonitor.sample） ---------- */

/* 单帧预算：与第 11 章的 16.7ms（60fps）一致。 */
#define AIC_PERF_FRAME_BUDGET_MS 16.7
/* 常驻内存环形采样容量：按秒推一次约可保存 32 秒曲线。 */
#define AIC_PERF_RING_MAX        32

typedef struct aic_perf_sample {
    long long at_ms;            /* 采样时刻（epoch 毫秒） */
    long long rss_kb;           /* 当前常驻内存（KB） */
    int       rss_ok;           /* 0 表示平台不支持采样 */
    double    frame_ms;         /* 最近一帧耗时 */
    double    frame_budget_ms;  /* 单帧预算 */
    int       frame_over;       /* 1 表示超出单帧预算 */
    double    mode_switch_ms;   /* 最近一次模式切换耗时 */
    double    tab_switch_ms;    /* 最近一次三栏切换耗时 */
    long long db_bytes;         /* 主库文件字节数 */
    long long wal_bytes;        /* -wal 与 -shm 字节数 */
    long long storage_kb;       /* 库相关文件合计（KB） */
} aic_perf_sample;

/* 清零并写入默认的单帧预算；采样前先调用。 */
void aic_perf_sample_init(aic_perf_sample *s);
/* 采一次：内存 + 存储分项；db 可为 NULL（只采内存），db_path 用于算存储。 */
int  aic_perf_sample_take(aic_db *db, const char *db_path, aic_perf_sample *s);
/* 记录单帧耗时，同时刷新 frame_over。 */
void aic_perf_sample_set_frame(aic_perf_sample *s, double frame_ms);
/* 记录模式切换 / 三栏切换耗时。 */
void aic_perf_sample_set_switch(aic_perf_sample *s, double mode_ms, double tab_ms);
/* 输出采样 JSON（开发版采样面板与 aic.exe perf 用它）。 */
int  aic_perf_sample_json(const aic_perf_sample *s, sb *out);

/* 常驻内存环形采样：界面每秒推一次，用于内存曲线与峰值。 */
void      aic_perf_ring_reset(void);
void      aic_perf_ring_push(long long rss_kb);
int       aic_perf_ring_count(void);
long long aic_perf_ring_avg_kb(void);
long long aic_perf_ring_peak_kb(void);

#ifdef __cplusplus
}
#endif

#endif /* AIC_PERF_H */
