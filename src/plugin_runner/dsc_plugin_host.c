/* dsc_plugin_host.c —— 插件宿主进程（独立进程，崩溃不牵连主程序）。
 *
 * 用法（由主程序启动，不给人直接调用）：
 *   dsc_plugin_host.exe <插件动态库路径> <请求 JSON 文件> <响应输出文件>
 *
 * 进程退出码：
 *   0  插件正常返回
 *   2  动态库里没有 dsc_plugin_call 符号
 *   3  动态库加载失败
 *   4  参数或文件读写失败
 * 插件自己崩溃（访问违例等）时不会是 0——父进程据此判为「插件进程异常退出」并禁用插件。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api.h"

#define HOST_OUT_CAP 65536

static int read_file_alloc(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 1;
}

static int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: dsc_plugin_host <dll> <request.json> <response.json>\n");
        return 4;
    }
    const char *dll_path = argv[1];
    const char *req_path = argv[2];
    const char *resp_path = argv[3];

    char *request = NULL;
    size_t request_len = 0;
    if (!read_file_alloc(req_path, &request, &request_len)) {
        fprintf(stderr, "read request failed: %s\n", req_path);
        return 4;
    }

    HMODULE mod = LoadLibraryA(dll_path);
    if (!mod) {
        fprintf(stderr, "LoadLibrary failed (%lu): %s\n", (unsigned long)GetLastError(), dll_path);
        free(request);
        return 3;
    }
    dsc_plugin_call_fn fn = (dsc_plugin_call_fn)(void (*)(void))GetProcAddress(mod, DSC_PLUGIN_ENTRY_SYMBOL);
    if (!fn) {
        fprintf(stderr, "symbol %s not found in %s\n", DSC_PLUGIN_ENTRY_SYMBOL, dll_path);
        FreeLibrary(mod);
        free(request);
        return 2;
    }

    char *out = (char *)calloc(1, HOST_OUT_CAP);
    if (!out) {
        FreeLibrary(mod);
        free(request);
        return 4;
    }
    int rc = fn(request, out, HOST_OUT_CAP);
    (void)rc;
    out[HOST_OUT_CAP - 1] = '\0';

    int ok = write_file(resp_path, out, strlen(out));
    free(out);
    free(request);
    FreeLibrary(mod);
    return ok ? 0 : 4;
}
