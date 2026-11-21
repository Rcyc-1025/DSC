/* credential_win.c —— Windows 凭据管理器实现（advapi32）。
 *
 * API Key 以 CRED_TYPE_GENERIC 存在当前用户凭据集里，只在进程内存中出现明文；
 * 日志、错误文本、导出包都不含它。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <stdio.h>
#include <string.h>

#include "credential.h"
#include "util.h"

static int to_wide(const char *src, wchar_t *dst, size_t cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    return n > 0;
}

static void win_err(char *err, size_t errlen, const char *what, DWORD code)
{
    if (!err || !errlen) return;
    snprintf(err, errlen, "%s（Windows 错误码 %lu）", what, (unsigned long)code);
}

int aic_cred_available(void) { return 1; }

int aic_cred_save(const char *service, const char *account, const char *secret,
                  char *err, size_t errlen)
{
    if (!service || !account || !secret) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';

    wchar_t wtarget[512], wuser[256];
    if (!to_wide(service, wtarget, 256) || !to_wide(account, wuser, 128)) {
        win_err(err, errlen, "凭据名称转码失败", GetLastError());
        return AIC_E_INVAL;
    }

    CREDENTIALW cred;
    memset(&cred, 0, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = wtarget;
    cred.UserName = wuser;
    cred.Comment = (LPWSTR)L"AIChat API Key";
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.CredentialBlobSize = (DWORD)strlen(secret);
    cred.CredentialBlob = (LPBYTE)secret;

    if (!CredWriteW(&cred, 0)) {
        win_err(err, errlen, "写入系统凭据失败", GetLastError());
        return AIC_E_IO;
    }
    return AIC_OK;
}

int aic_cred_load(const char *service, const char *account, char *out, size_t cap,
                  char *err, size_t errlen)
{
    if (!service || !account || !out || cap == 0) return AIC_E_INVAL;
    out[0] = '\0';
    if (err && errlen) err[0] = '\0';

    wchar_t wtarget[512];
    if (!to_wide(service, wtarget, 256)) {
        win_err(err, errlen, "凭据名称转码失败", GetLastError());
        return AIC_E_INVAL;
    }

    PCREDENTIALW cred = NULL;
    if (!CredReadW(wtarget, CRED_TYPE_GENERIC, 0, &cred)) {
        DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) {
            if (err && errlen) aic_strlcpy_utf8(err, errlen, "尚未配置 API Key");
            return AIC_E_NOTFOUND;
        }
        win_err(err, errlen, "读取系统凭据失败", code);
        return AIC_E_IO;
    }

    int rc = AIC_OK;
    size_t n = cred->CredentialBlobSize;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(out, cred->CredentialBlob, n);
    out[n] = '\0';
    if (n == 0) rc = AIC_E_NOTFOUND;
    CredFree(cred);
    return rc;
}

int aic_cred_delete(const char *service, const char *account, char *err, size_t errlen)
{
    (void)account;
    if (!service) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    wchar_t wtarget[512];
    if (!to_wide(service, wtarget, 256)) return AIC_E_INVAL;
    if (!CredDeleteW(wtarget, CRED_TYPE_GENERIC, 0)) {
        DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) return AIC_E_NOTFOUND;
        win_err(err, errlen, "删除系统凭据失败", code);
        return AIC_E_IO;
    }
    return AIC_OK;
}
