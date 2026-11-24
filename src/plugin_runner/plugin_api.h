/* plugin_api.h —— 插件与宿主之间的纯 C ABI（Windows DLL / Android .so 通用）。
 *
 * 插件（动态库）必须导出下面这个符号：
 *
 *   int dsc_plugin_call(const char *request_json, char *out, size_t out_cap);
 *
 * 约定：
 *   - request_json 是宿主传入的请求（UTF-8 JSON）；
 *   - 返回值写进 out（UTF-8，NUL 结尾），返回值 < out_cap；
 *   - 返回 DSC_OK(0) 表示成功，其它值为插件自己的错误码（宿主只做记录）；
 *   - 插件不允许自己读写核心数据目录，需要什么都在请求里给。
 */
#ifndef DSC_PLUGIN_API_H
#define DSC_PLUGIN_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_PLUGIN_ENTRY_SYMBOL "dsc_plugin_call"

typedef int (*dsc_plugin_call_fn)(const char *request_json, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DSC_PLUGIN_API_H */
