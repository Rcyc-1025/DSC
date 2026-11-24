/* notify_win.c —— 系统通知（Windows）：托盘气泡。
 * 用 Shell_NotifyIconW 先 NIM_ADD 建图标、再 NIM_MODIFY 带 NIF_INFO 弹气泡，
 * 不依赖 WinRT/Toast，便携包也能用。
 */
#include "notify.h"

#include <windows.h>
#include <shellapi.h>

#include <string.h>

#include "util.h"

#define NOTIFY_ICON_ID 1

static void to_wide(const char *in, WCHAR *out, int cap)
{
    if (!in) in = "";
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
    out[cap - 1] = 0;
}

int dsc_notify_supported(void) { return 1; }

int dsc_notify(void *window, const char *title, const char *body)
{
    if (!window) return DSC_E_INVAL;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = (HWND)window;
    nid.uID = NOTIFY_ICON_ID;
    /* 先登记托盘图标（已存在时 NIM_ADD 会失败，忽略即可）。 */
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP + 7;
    nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    to_wide("DSC", nid.szTip, (int)(sizeof(nid.szTip) / sizeof(WCHAR)));
    Shell_NotifyIconW(NIM_ADD, &nid);
    /* 再弹气泡通知。 */
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = (HWND)window;
    nid.uID = NOTIFY_ICON_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    to_wide(title, nid.szInfoTitle, (int)(sizeof(nid.szInfoTitle) / sizeof(WCHAR)));
    to_wide(body, nid.szInfo, (int)(sizeof(nid.szInfo) / sizeof(WCHAR)));
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) return DSC_E_UNSUPPORTED;
    return DSC_OK;
}
