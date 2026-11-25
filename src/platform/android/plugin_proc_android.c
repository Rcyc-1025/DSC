/* plugin_proc_android.c —— Android 上「插件子进程隔离」的平台实现（FR-05）。
 *
 * Android 没有 dsc_plugin_host 这个独立可执行文件：插件由 Java 侧 PluginHostService
 * 在 :plugin 进程里再起一个一次性的 PluginWorker 进程执行，核心层只负责汇总回灌
 * （见 android/app/src/main/cpp/dsc_jni.c 的 cmd_plugin_call_point）。
 * 所以界面路径永远走不到这里的 dsc_plugin_proc_call。
 *
 * 但它必须存在：core/plugin.c 的 call_one 无条件引用 dsc_plugin_proc_call / _free，
 * 共享库允许留下未定义符号，编译和链接都不报错，直到 Android 的 dlopen 逐个解析符号才
 * 失败 —— 缺一个就整个 libdsc.so 加载不起来（0.1.1 在真机上正是这么退的）。
 * 因此这里给出真实实现：available 返回 0（与 plugin_proc.h 的口径一致），
 * call 明确返回 DSC_E_UNSUPPORTED，绝不假装成功。
 */
#include <string.h>

#include "dsc_types.h"
#include "plugin_proc.h"
#include "util.h"

int dsc_plugin_proc_available(void) { return 0; }

int dsc_plugin_proc_call(const char *host_exe, const char *entry_path,
                         const char *request_json, size_t request_len,
                         int timeout_ms, const char *work_dir,
                         dsc_plugin_proc_result *out, char *err, size_t errlen)
{
    /* 未使用的参数保持显式（-Wall -Wextra 下也不报 unused-parameter 是因为构建带了关闭项，
     * 这里仍逐个 (void) 掉，避免以后去掉该关闭项时又冒警告）。 */
    (void)host_exe;
    (void)entry_path;
    (void)request_json;
    (void)request_len;
    (void)timeout_ms;
    (void)work_dir;
    if (out) memset(out, 0, sizeof(*out));
    if (err && errlen) {
        dsc_strlcpy_utf8(err, errlen,
                         "Android 上插件由 Java 侧的独立进程执行，核心层不直接起子进程");
    }
    return DSC_E_UNSUPPORTED;
}

void dsc_plugin_proc_free(dsc_plugin_proc_result *r)
{
    if (!r) return;
    dsc_xfree(r->text);
    r->text = NULL;
    r->len = 0;
}
