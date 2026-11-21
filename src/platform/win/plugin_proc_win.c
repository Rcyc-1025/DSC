/* plugin_proc_win.c —— Windows 下的插件子进程调用（CreateProcess + 超时终止）。 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "plugin_proc.h"
#include "util.h"

#define PROC_DEFAULT_TIMEOUT_MS 5000

int aic_plugin_proc_available(void) { return 1; }

static int wide_from_utf8(const char *s, wchar_t *out, size_t cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap);
    return n > 0;
}

int aic_plugin_proc_call(const char *host_exe, const char *entry_path,
                         const char *request_json, size_t request_len,
                         int timeout_ms, const char *work_dir,
                         aic_plugin_proc_result *out, char *err, size_t errlen)
{
    if (!host_exe || !entry_path || !request_json || !out) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    if (timeout_ms <= 0) timeout_ms = PROC_DEFAULT_TIMEOUT_MS;

    if (!aic_path_exists(host_exe)) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件宿主程序不存在");
        return AIC_E_NOTFOUND;
    }
    if (!aic_path_exists(entry_path)) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件入口文件不存在");
        return AIC_E_NOTFOUND;
    }

    char uuid[40];
    aic_uuid4(uuid);
    char req_name[80], resp_name[80];
    snprintf(req_name, sizeof(req_name), "plgreq_%s.json", uuid);
    snprintf(resp_name, sizeof(resp_name), "plgresp_%s.json", uuid);

    char req_path[1024], resp_path[1024];
    if (work_dir && work_dir[0]) {
        aic_path_join(req_path, sizeof(req_path), work_dir, req_name);
        aic_path_join(resp_path, sizeof(resp_path), work_dir, resp_name);
    } else {
        aic_strlcpy_utf8(req_path, sizeof(req_path), req_name);
        aic_strlcpy_utf8(resp_path, sizeof(resp_path), resp_name);
    }

    int rc = aic_file_write_all(req_path, request_json, request_len);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "写入插件请求文件失败");
        return rc;
    }

    wchar_t whost[MAX_PATH * 2], wentry[MAX_PATH * 2], wreq[MAX_PATH * 2], wresp[MAX_PATH * 2];
    if (!wide_from_utf8(host_exe, whost, MAX_PATH) ||
        !wide_from_utf8(entry_path, wentry, MAX_PATH) ||
        !wide_from_utf8(req_path, wreq, MAX_PATH) ||
        !wide_from_utf8(resp_path, wresp, MAX_PATH)) {
        aic_file_remove(req_path);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "路径包含无法转换的字符");
        return AIC_E_INVAL;
    }

    wchar_t cmdline[8192];
    _snwprintf(cmdline, 8192, L"\"%ls\" \"%ls\" \"%ls\" \"%ls\"", whost, wentry, wreq, wresp);
    cmdline[8191] = L'\0';

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    long long started = aic_now_ms();
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        aic_file_remove(req_path);
        if (err && errlen) snprintf(err, errlen, "启动插件进程失败（Windows 错误码 %lu）", (unsigned long)GetLastError());
        return AIC_E_STATE;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        aic_file_remove(req_path);
        aic_file_remove(resp_path);
        out->timed_out = 1;
        out->exit_code = -1;
        out->elapsed_ms = aic_now_ms() - started;
        if (err && errlen) snprintf(err, errlen, "插件超时（超过 %d 毫秒），已终止插件进程", timeout_ms);
        return AIC_E_TIMEOUT;
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    aic_file_remove(req_path);

    out->exit_code = (int)code;
    out->elapsed_ms = aic_now_ms() - started;

    if (code != 0) {
        aic_file_remove(resp_path);
        if (err && errlen) {
            if (code == 2) aic_strlcpy_utf8(err, errlen, "插件缺少 aic_plugin_call 入口，已禁用该插件");
            else if (code == 3) aic_strlcpy_utf8(err, errlen, "插件动态库加载失败，已禁用该插件");
            else snprintf(err, errlen, "插件进程异常退出（退出码 %lu），主程序继续运行", (unsigned long)code);
        }
        return AIC_E_STATE;
    }

    char *text = NULL;
    size_t len = 0;
    rc = aic_file_read_all(resp_path, &text, &len);
    aic_file_remove(resp_path);
    if (rc != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "插件没有写出响应");
        return rc;
    }
    out->text = text;
    out->len = len;
    return AIC_OK;
}

void aic_plugin_proc_free(aic_plugin_proc_result *r)
{
    if (!r) return;
    aic_xfree(r->text);
    r->text = NULL;
    r->len = 0;
}
