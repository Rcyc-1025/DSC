/* plugin_proc.h —— 把插件放进独立子进程执行（FR-05，技术设计 §8 进程与线程）。
 *
 * 主程序不直接 LoadLibrary 用户的插件：插件在自己进程里跑，崩溃、卡死、泄漏都只影响那个进程。
 * 超时由宿主终止子进程并返回 DSC_E_TIMEOUT；插件异常退出返回 DSC_E_STATE。
 * 平台实现：Windows 见 src/platform/win/plugin_proc_win.c。
 */
#ifndef DSC_PLUGIN_PROC_H
#define DSC_PLUGIN_PROC_H

#include <stddef.h>

#include "dsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsc_plugin_proc_result {
    char     *text;        /* 插件返回的 JSON 文本（堆分配，需 dsc_plugin_proc_free） */
    size_t    len;
    int       exit_code;   /* 子进程退出码；被超时终止时为 -1 */
    int       timed_out;   /* 1 表示超时被终止 */
    long long elapsed_ms;  /* 整个回合的墙钟毫秒 */
    long long peak_mem_kb; /* 子进程峰值内存（KB），用于判断是否是内存上限触发 */
    long long cpu_ms;      /* 子进程用户态 CPU 时间（毫秒，含 CPU 上限阈值） */
} dsc_plugin_proc_result;

/* 在独立进程里调用插件入口。
 *   host_exe     插件宿主程序（build 产物 dsc_plugin_host.exe）
 *   entry_path   插件动态库绝对/相对路径
 *   request_json 请求内容（UTF-8）
 *   timeout_ms   超时毫秒，<=0 取默认 5000
 *   work_dir     放临时请求/响应文件的目录（必须存在）
 * 返回：DSC_OK（插件正常返回）/ DSC_E_TIMEOUT / DSC_E_STATE（插件进程异常退出）/ DSC_E_NOTFOUND（宿主或插件文件缺失） */
int dsc_plugin_proc_call(const char *host_exe, const char *entry_path,
                         const char *request_json, size_t request_len,
                         int timeout_ms, const char *work_dir,
                         dsc_plugin_proc_result *out, char *err, size_t errlen);

void dsc_plugin_proc_free(dsc_plugin_proc_result *r);

/* 当前平台是否支持子进程隔离（Android 侧未实现时返回 0，界面据此禁用插件）。 */
int dsc_plugin_proc_available(void);

#ifdef __cplusplus
}
#endif

#endif /* DSC_PLUGIN_PROC_H */
