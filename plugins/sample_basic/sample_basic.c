/*
 * 示例插件：演示最小可用的纯 C ABI 插件。
 * 编译：gcc -std=c11 -O2 -shared -o sample_basic.dll sample_basic.c -Isrc/plugin_runner
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "plugin_api.h"

static int sample_call(const char *request_json, char *out, size_t out_cap)
{
    int n;
    if (!request_json || !out || out_cap < 32) return -1;
    n = snprintf(out, out_cap,
                 "{\"plugin\":\"sample_basic\",\"request_len\":%d}",
                 (int)strlen(request_json));
    if (n <= 0 || (size_t)n >= out_cap) return -1;
    return 0;
}

__declspec(dllexport) int dsc_plugin_call(const char *request_json, char *out, size_t out_cap)
{
    return sample_call(request_json, out, out_cap);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
