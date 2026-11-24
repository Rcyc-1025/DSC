/* http.h —— 平台 HTTP 能力：流式对话传输、断点续传下载、账户查询。
 *
 * 平台实现：
 *   src/platform/win/http_win.c   Windows（WinHTTP，系统组件，不引第三方库）
 *   android/app/src/main/cpp/dsc_jni.c  Android（由 Java 侧 HttpURLConnection 完成，
 *                                     C 侧通过 begin/feed/end 三段协议喂 SSE 解析器）
 * 没有平台实现时（例如在别的系统上编译核心层）：dsc_http_available() 返回 0，
 * 所有调用返回明确错误，绝不假装成功。 */
#ifndef DSC_HTTP_H
#define DSC_HTTP_H

#include <stddef.h>

#include "dsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 进度回调：got/total（total<=0 表示服务端没给总长度）。 */
typedef void (*dsc_http_progress_fn)(void *ud, long long got, long long total);

/* 当前平台是否有可用的 HTTP 实现（Windows 上恒为 1）。 */
int dsc_http_available(void);

/* 断点续传下载：把 url 落到 dst。
 * resume=1 且 dst 已有内容时用 Range 续传；服务端不支持 206 则整体重下。
 * max_bytes>0 时超过即中止（防止被超大响应打满磁盘）。cancel 非空且置 1 时尽快中止。 */
int dsc_http_download(const char *url, const char *dst, long long max_bytes, int resume,
                      dsc_http_progress_fn prog, void *prog_ud, volatile int *cancel,
                      char *err, size_t errlen);

/* GET 一个小 JSON（余额、更新清单）：正文堆分配，调用方 dsc_xfree。
 * http_status 返回 HTTP 状态码，便于调用方按 401/429/5xx 分类。 */
int dsc_http_get_json(const char *url, const char *api_key, char **out_body,
                      int *http_status, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DSC_HTTP_H */
