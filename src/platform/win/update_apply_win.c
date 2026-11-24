/* update_apply_win.c —— Windows 替换安装：回滚点 → 静默安装 → 失败还原。
 *
 * 安装包是本项目自己的自解压安装程序（执行 dist\DSC-<ver>-win-x64-setup.exe /S /D=<目录>）。
 * 替换前把当前程序目录整体复制到 backups\app-<时间戳>，安装退出码非 0 时整目录还原。 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "update.h"
#include "util.h"

static void to_wide(const char *in, WCHAR *out, int cap)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
    out[cap - 1] = 0;
}

/* 递归复制目录（dst 必须不存在或为空）。 */
static int copy_tree(const char *src, const char *dst)
{
    int rc = dsc_mkdir_p(dst);
    if (rc != DSC_OK) return rc;
    sb names;
    sb_init(&names);
    int n = dsc_dir_list(src, &names, '\n');
    if (n > 0) {
        const char *p = sb_cstr(&names);
        size_t len = strlen(p), start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || p[i] == '\n') {
                if (i > start) {
                    char name[256];
                    size_t k = i - start;
                    if (k >= sizeof(name)) k = sizeof(name) - 1;
                    memcpy(name, p + start, k);
                    name[k] = '\0';
                    char s[1024], d[1024];
                    dsc_path_join(s, sizeof(s), src, name);
                    dsc_path_join(d, sizeof(d), dst, name);
                    if (dsc_is_dir(s)) copy_tree(s, d);
                    else {
                        int r = dsc_file_copy(s, d);
                        if (r != DSC_OK) { sb_free(&names); return r; }
                    }
                }
                start = i + 1;
            }
        }
    }
    sb_free(&names);
    return DSC_OK;
}

int dsc_update_apply_package(const char *pkg, const char *app_dir, const char *backup_dir,
                             volatile int *cancel, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!pkg || !app_dir || !backup_dir) return DSC_E_INVAL;
    if (!dsc_path_exists(pkg)) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "安装包不存在");
        return DSC_E_NOTFOUND;
    }
    if (cancel && *cancel) return DSC_E_TIMEOUT;

    char rollback[1024];
    snprintf(rollback, sizeof(rollback), "%s/app-%lld", backup_dir, (long long)dsc_now_ms());
    dsc_mkdir_p(backup_dir);
    int rc = copy_tree(app_dir, rollback);
    if (rc != DSC_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "创建回滚点失败，已中止更新");
        return rc;
    }

    WCHAR wpkg[1024], wdir[1024], cmd[2200];
    to_wide(pkg, wpkg, 1024);
    to_wide(app_dir, wdir, 1024);
    _snwprintf(cmd, 2200, L"\"%ls\" /S /D=%ls", wpkg, wdir);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (err && errlen) snprintf(err, errlen, "启动安装程序失败（错误码 %lu）", GetLastError());
        return DSC_E_STATE;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        code = 1;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code != 0) {
        if (cancel && *cancel) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "更新已取消，程序目录保持原样");
            return DSC_E_TIMEOUT;
        }
        /* 失败回滚：把回滚点覆盖回去。 */
        copy_tree(rollback, app_dir);
        if (err && errlen) snprintf(err, errlen, "安装程序返回 %lu，已回滚到更新前的版本", (unsigned long)code);
        return DSC_E_STATE;
    }
    return DSC_OK;
}
