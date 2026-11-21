/* diaglog.c */
#include "diaglog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

static char g_dir[512];
static char g_path[600];
static FILE *g_fp;
static long long g_bytes;

const char *aic_log_path(void) { return g_path; }

void aic_log_close(void)
{
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    g_bytes = 0;
}

static void open_day(void)
{
    aic_log_close();
    char stamp[32];
    aic_now_iso8601(stamp);            /* 2026-11-22T03:00:00.000Z */
    char day[16];
    memcpy(day, stamp, 10);
    day[10] = '\0';
    char name[64];
    snprintf(name, sizeof(name), "app-%s.log", day);
    if (aic_path_join(g_path, sizeof(g_path), g_dir, name) != AIC_OK) { g_path[0] = '\0'; return; }
    g_fp = fopen(g_path, "ab");
    if (!g_fp) { g_path[0] = '\0'; return; }
    int64_t sz = 0;
    if (aic_file_size(g_path, &sz) == AIC_OK) g_bytes = (long long)sz;
}

int aic_log_init(const char *dir)
{
    if (!dir || !*dir) return AIC_E_INVAL;
    if (aic_str_ieq(dir, g_dir) && g_fp) return AIC_OK;
    aic_log_close();
    aic_strlcpy_utf8(g_dir, sizeof(g_dir), dir);
    int rc = aic_mkdir_p(g_dir);
    if (rc != AIC_OK) { g_path[0] = '\0'; return rc; }
    open_day();
    return g_fp ? AIC_OK : AIC_E_IO;
}

static void rotate_if_needed(void)
{
    if (!g_fp) return;
    if (g_bytes < AIC_LOG_ROTATE_BYTES) return;
    aic_log_close();
    char stamp[32];
    aic_now_iso8601(stamp);
    char name[80];
    snprintf(name, sizeof(name), "app-%s.log", stamp);
    for (char *p = name; *p; p++) if (*p == ':') *p = '-';
    if (aic_path_join(g_path, sizeof(g_path), g_dir, name) != AIC_OK) return;
    g_fp = fopen(g_path, "ab");
    g_bytes = 0;
}

void aic_log_write(const char *level, const char *fmt, ...)
{
    if (!g_fp) return;
    rotate_if_needed();
    if (!g_fp) return;
    char stamp[32];
    aic_now_iso8601(stamp);
    int n = fprintf(g_fp, "%s [%s] ", stamp, level ? level : "INFO");
    va_list ap;
    va_start(ap, fmt);
    n += vfprintf(g_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_fp);
    fflush(g_fp);
    if (n > 0) g_bytes += n;
}

void aic_log_blob(const char *tag, const char *data, size_t len)
{
    if (!g_fp || !data) return;
    const size_t cap = 8192;
    size_t use = len < cap ? len : cap;
    aic_log_write("RESP", "%s（%lu 字节%s）", tag ? tag : "响应",
                  (unsigned long)len, len > cap ? "，已截断" : "");
    size_t start = 0;
    while (start < use) {
        size_t end = start;
        while (end < use && data[end] != '\n' && data[end] != '\r') end++;
        size_t seg = end - start;
        if (seg > AIC_LOG_MAX_LINE) seg = AIC_LOG_MAX_LINE;
        char line[AIC_LOG_MAX_LINE + 1];
        memcpy(line, data + start, seg);
        line[seg] = '\0';
        sb hit;
        sb_init(&hit);
        int sensitive = aic_scan_sensitive(line, &hit);
        sb_free(&hit);
        if (sensitive) {
            if (g_fp) { fprintf(g_fp, "  | [已脱敏：该行含敏感串]\n"); fflush(g_fp); }
        } else {
            if (g_fp) { fprintf(g_fp, "  | %s\n", line); fflush(g_fp); }
        }
        start = end;
        while (start < use && (data[start] == '\n' || data[start] == '\r')) start++;
    }
}

void aic_log_error(const char *where, int code, const char *err)
{
    aic_log_write("ERROR", "%s 失败（%d %s）：%s", where ? where : "操作", code,
                  aic_strerror(code), err ? err : "");
}
