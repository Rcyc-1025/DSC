/* crash_plugin.c —— 测试用崩溃插件：故意访问空指针。
 * 宿主必须只让它的进程死掉，主程序继续运行并禁用该插件。 */
#include "plugin_api.h"

int aic_plugin_call(const char *request_json, char *out, size_t out_cap)
{
    (void)request_json; (void)out; (void)out_cap;
    volatile int *p = (volatile int *)0;
    *p = 1;             /* 触发访问违例 */
    return 0;
}
