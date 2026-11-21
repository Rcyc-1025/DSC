/* perf.c —— 性能预算与基准实现。 */
#include "perf.h"
#include "json.h"
#include "store_message.h"
#include "transfer.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#endif

/* 预算表：与 §13.2 一一对应（桌面 / 移动）。 */
typedef struct budget_row {
    aic_perf_metric metric;
    double desktop;
    double mobile;
} budget_row;

static const budget_row g_budget[] = {
    { AIC_PERF_COLD_START,   2000.0, 2500.0 },
    { AIC_PERF_MEMORY_IDLE,   240.0,  180.0 },
    { AIC_PERF_MODE_SWITCH,   200.0,  220.0 },
    { AIC_PERF_TAB_SWITCH,    150.0,  160.0 },
    { AIC_PERF_FIRST_TOKEN,  2000.0, 2000.0 },
    { AIC_PERF_PAGE_10K,      300.0,  350.0 },
    { AIC_PERF_EXPORT_10K,   2000.0, 2500.0 }
};

double aic_perf_budget_value(aic_platform platform, aic_perf_metric metric)
{
    for (size_t i = 0; i < sizeof(g_budget) / sizeof(g_budget[0]); i++) {
        if (g_budget[i].metric != metric) continue;
        return platform == AIC_PLATFORM_MOBILE ? g_budget[i].mobile : g_budget[i].desktop;
    }
    return 0.0;
}

int aic_perf_budget(aic_platform platform, aic_perf_metric metric, double *out)
{
    double v = aic_perf_budget_value(platform, metric);
    if (v <= 0.0) return AIC_E_INVAL;
    if (out) *out = v;
    return AIC_OK;
}

int aic_perf_check(aic_platform platform, aic_perf_metric metric, double value, double *out_over)
{
    double limit = aic_perf_budget_value(platform, metric);
    if (limit <= 0.0) {
        if (out_over) *out_over = 0.0;
        return 0;
    }
    double over = (value - limit) / limit * 100.0;
    if (out_over) *out_over = over;
    return value <= limit ? 1 : 0;
}

const char *aic_perf_metric_name(aic_perf_metric m)
{
    switch (m) {
    case AIC_PERF_COLD_START:  return "冷启动";
    case AIC_PERF_MEMORY_IDLE: return "常驻内存";
    case AIC_PERF_MODE_SWITCH: return "模式切换";
    case AIC_PERF_TAB_SWITCH:  return "三栏切换";
    case AIC_PERF_FIRST_TOKEN: return "首段回复";
    case AIC_PERF_PAGE_10K:    return "万条分页";
    case AIC_PERF_EXPORT_10K:  return "万条导出";
    default:                   return "未知指标";
    }
}

const char *aic_perf_metric_unit(aic_perf_metric m)
{
    return m == AIC_PERF_MEMORY_IDLE ? "MB" : "ms";
}

long long aic_perf_now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c)) return 0;
    return (long long)((double)c.QuadPart * 1000000.0 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

double aic_perf_now_ms(void) { return (double)aic_perf_now_us() / 1000.0; }

int aic_perf_rss_kb(long long *out_kb)
{
    if (!out_kb) return AIC_E_INVAL;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return AIC_E_UNSUPPORTED;
    *out_kb = (long long)(pmc.WorkingSetSize / 1024);
    return AIC_OK;
#else
    FILE *f = fopen("/proc/self/statm", "rb");
    if (!f) return AIC_E_UNSUPPORTED;
    long long total = 0, rss = 0;
    int n = fscanf(f, "%lld %lld", &total, &rss);
    fclose(f);
    if (n != 2) return AIC_E_UNSUPPORTED;
    *out_kb = rss * 4;   /* 页大小按 4KB */
    return AIC_OK;
#endif
}

int aic_perf_bench_page(aic_db *db, const char *conv_id, int pages, int page_size,
                        double *out_ms)
{
    if (!db || !conv_id || !out_ms) return AIC_E_INVAL;
    if (pages <= 0) pages = 1;
    if (page_size <= 0) page_size = 50;
    long long t0 = aic_perf_now_us();
    long long before_seq = 0;
    for (int i = 0; i < pages; i++) {
        aic_message *arr = NULL;
        size_t n = 0;
        int rc = aic_msg_page(db, conv_id, before_seq, page_size, &arr, &n);
        if (rc != AIC_OK) return rc;
        if (n > 0) before_seq = arr[0].seq;
        aic_msg_free_array(arr, n);
        if (n == 0) break;
    }
    long long t1 = aic_perf_now_us();
    *out_ms = (double)(t1 - t0) / 1000.0 / (double)pages;
    return AIC_OK;
}

int aic_perf_bench_export(aic_db *db, int mode, double *out_ms)
{
    if (!db || !out_ms) return AIC_E_INVAL;
    sb out;
    sb_init(&out);
    char err[256];
    long long t0 = aic_perf_now_us();
    int rc = aic_export_json(db, mode, &out, err, sizeof(err));
    long long t1 = aic_perf_now_us();
    size_t bytes = out.len;
    sb_free(&out);
    /* 复用 out.len 只是为了消除"未使用"警告，导出耗时才是返回值。 */
    (void)bytes;
    if (rc != AIC_OK) return rc;
    *out_ms = (double)(t1 - t0) / 1000.0;
    return AIC_OK;
}

int aic_perf_report(aic_platform platform, sb *out)
{
    if (!out) return AIC_E_INVAL;
    sb_reset(out);
    sb_appendf(out, "性能预算（%s）\n", platform == AIC_PLATFORM_MOBILE ? "移动端" : "桌面端");
    for (int i = 0; i < (int)AIC_PERF_METRIC_COUNT; i++) {
        double v = aic_perf_budget_value(platform, (aic_perf_metric)i);
        sb_appendf(out, "  %s：不超过 %.1f %s\n", aic_perf_metric_name((aic_perf_metric)i), v,
                   aic_perf_metric_unit((aic_perf_metric)i));
    }
    long long rss = 0;
    if (aic_perf_rss_kb(&rss) == AIC_OK) {
        sb_appendf(out, "  当前进程常驻内存：%.1f MB\n", (double)rss / 1024.0);
    }
    return AIC_OK;
}

/* ---------- 采样（PerfMonitor.sample） ---------- */

static long long g_ring[AIC_PERF_RING_MAX];
static int       g_ring_n;
static int       g_ring_pos;

void aic_perf_ring_reset(void)
{
    memset(g_ring, 0, sizeof(g_ring));
    g_ring_n = 0;
    g_ring_pos = 0;
}

void aic_perf_ring_push(long long rss_kb)
{
    g_ring[g_ring_pos] = rss_kb;
    g_ring_pos = (g_ring_pos + 1) % AIC_PERF_RING_MAX;
    if (g_ring_n < AIC_PERF_RING_MAX) g_ring_n++;
}

int aic_perf_ring_count(void)
{
    return g_ring_n;
}

long long aic_perf_ring_avg_kb(void)
{
    long long sum = 0;
    if (g_ring_n <= 0) return 0;
    for (int i = 0; i < g_ring_n; i++) sum += g_ring[i];
    return sum / g_ring_n;
}

long long aic_perf_ring_peak_kb(void)
{
    long long peak = 0;
    for (int i = 0; i < g_ring_n; i++) {
        if (g_ring[i] > peak) peak = g_ring[i];
    }
    return peak;
}

void aic_perf_sample_init(aic_perf_sample *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->frame_budget_ms = AIC_PERF_FRAME_BUDGET_MS;
}

static long long size_of_or_zero(const char *path)
{
    int64_t n = 0;
    if (!path || !path[0]) return 0;
    if (aic_file_size(path, &n) != AIC_OK) return 0;
    return (long long)n;
}

int aic_perf_sample_take(aic_db *db, const char *db_path, aic_perf_sample *s)
{
    /* 采样不读库内容，只按路径量文件大小，故 db 不参与计算。 */
    (void)db;
    if (!s) return AIC_E_INVAL;
    aic_perf_sample_init(s);
    s->at_ms = aic_now_ms();

    long long kb = 0;
    if (aic_perf_rss_kb(&kb) == AIC_OK) {
        s->rss_ok = 1;
        s->rss_kb = kb;
        aic_perf_ring_push(kb);
    }
    if (db_path && db_path[0]) {
        char tmp[1600];
        s->db_bytes = size_of_or_zero(db_path);
        snprintf(tmp, sizeof(tmp), "%s-wal", db_path);
        s->wal_bytes += size_of_or_zero(tmp);
        snprintf(tmp, sizeof(tmp), "%s-shm", db_path);
        s->wal_bytes += size_of_or_zero(tmp);
        s->storage_kb = (s->db_bytes + s->wal_bytes) / 1024;
    }
    return AIC_OK;
}

void aic_perf_sample_set_frame(aic_perf_sample *s, double frame_ms)
{
    if (!s) return;
    s->frame_ms = frame_ms;
    s->frame_over = (s->frame_budget_ms > 0.0 && frame_ms > s->frame_budget_ms) ? 1 : 0;
}

void aic_perf_sample_set_switch(aic_perf_sample *s, double mode_ms, double tab_ms)
{
    if (!s) return;
    if (mode_ms >= 0.0) s->mode_switch_ms = mode_ms;
    if (tab_ms >= 0.0) s->tab_switch_ms = tab_ms;
}

int aic_perf_sample_json(const aic_perf_sample *s, sb *out)
{
    if (!s || !out) return AIC_E_INVAL;
    jw w;
    jw_init(&w, out, 1);
    jw_obj_begin(&w, NULL);
    jw_put_int(&w, "at_ms", s->at_ms);
    jw_put_int(&w, "rss_kb", s->rss_kb);
    jw_put_bool(&w, "rss_ok", s->rss_ok);
    jw_put_double(&w, "rss_mb", (double)s->rss_kb / 1024.0);
    jw_put_double(&w, "frame_ms", s->frame_ms);
    jw_put_double(&w, "frame_budget_ms", s->frame_budget_ms);
    jw_put_bool(&w, "frame_over", s->frame_over);
    jw_put_double(&w, "mode_switch_ms", s->mode_switch_ms);
    jw_put_double(&w, "tab_switch_ms", s->tab_switch_ms);
    jw_put_int(&w, "db_bytes", s->db_bytes);
    jw_put_int(&w, "wal_bytes", s->wal_bytes);
    jw_put_int(&w, "storage_kb", s->storage_kb);
    /* 环形缓冲一旦写满，数组顺序是"最新在前"的循环序，只用于看趋势与峰值。 */
    jw_arr_begin(&w, "rss_ring_kb");
    for (int i = 0; i < g_ring_n; i++) jw_put_int(&w, NULL, g_ring[i]);
    jw_arr_end(&w);
    jw_put_int(&w, "rss_avg_kb", aic_perf_ring_avg_kb());
    jw_put_int(&w, "rss_peak_kb", aic_perf_ring_peak_kb());
    jw_obj_end(&w);
    return AIC_OK;
}
