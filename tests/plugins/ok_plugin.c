/* ok_plugin.c —— 测试用的正常插件：把请求原样回执。 */
#include <stdio.h>
#include <string.h>

#include "plugin_api.h"

int aic_plugin_call(const char *request_json, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return -1;
    const char *req = request_json ? request_json : "";
    snprintf(out, out_cap, "{\"plugin\":\"ok\",\"request_len\":%d,\"echo\":\"%s\"}",
             (int)strlen(req), req);
    return 0;
}
