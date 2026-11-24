/* slow_plugin.c —— 测试用卡死插件：睡 5 秒，宿主超时后必须终止进程。 */
#include <windows.h>

#include "plugin_api.h"

int dsc_plugin_call(const char *request_json, char *out, size_t out_cap)
{
    (void)request_json;
    Sleep(5000);
    if (out && out_cap) out[0] = '\0';
    return 0;
}
