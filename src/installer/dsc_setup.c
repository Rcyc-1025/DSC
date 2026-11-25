/* dsc_setup.c —— DSC 安装程序（纯 C，Win32，无第三方运行时）。
 *
 * 形态：自解压式安装包。构建时由 tools/make_installer.py 把本程序编成 setup 桩，
 * 再把 payload（安装目录要落地的文件）以本文件约定的归档格式追加到 exe 末尾：
 *
 *     "AICSETUP-PAYLOAD-V1"  (19 字节标记)
 *     uint32  file_count
 *     每条：uint32 path_len | path(UTF-8, 相对安装目录) | uint64 size | data
 *
 * 运行：默认装到 %LOCALAPPDATA%\Programs\DSC，创建开始菜单快捷方式与卸载登记；
 *       /S 静默；/D=<目录> 指定安装目录。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MARKER "AICSETUP-PAYLOAD-V1"
#define MARKER_LEN (sizeof(MARKER) - 1)
#define APP_NAME L"DSC"
#define APP_VERSION L"0.1.4"
#define UNINST_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DSC"
#define INSTALLER_VERSION "DSC Setup 0.1.4"

static int g_silent = 0;
static WCHAR g_dir[MAX_PATH * 2];
static WCHAR g_exe[MAX_PATH * 2];

/* 诊断日志：写到 %TEMP%\DscSetup.log，只在排查安装问题时用；写失败不影响安装。 */
static void setup_log(const char *fmt, ...)
{
    char line[1024];
    WCHAR tmp[MAX_PATH];
    WCHAR dbg[8];
    WCHAR path[MAX_PATH + 32];
    va_list ap;
    int n;
    if (GetEnvironmentVariableW(L"DSC_SETUP_DEBUG", dbg, 8) == 0) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (GetTempPathW(MAX_PATH, tmp) == 0) return;
    _snwprintf(path, MAX_PATH + 31, L"%sDscSetup.log", tmp);
    path[MAX_PATH + 31] = L'\0';
    n = (int)strlen(line);
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    {
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        if (h == INVALID_HANDLE_VALUE) return;
        line[n++] = '\r';
        line[n++] = '\n';
        WriteFile(h, line, (DWORD)n, &written, NULL);
        CloseHandle(h);
    }
}

static void msg(const WCHAR *text, UINT flags)
{
    if (!g_silent) MessageBoxW(NULL, text, APP_NAME, flags);
}

/* UTF-8 字节转宽字符（安装包里路径统一 UTF-8）。 */
static void u8_to_w(const char *in, WCHAR *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
    if (n <= 0 && cap > 0) out[0] = L'\0';
}

/* 递归创建目录（中间层缺失也补上）。 */
static int mkdir_deep(const WCHAR *path)
{
    WCHAR buf[MAX_PATH * 2];
    size_t i, len;
    if (!path || !path[0]) return 0;
    wcsncpy(buf, path, MAX_PATH * 2 - 1);
    buf[MAX_PATH * 2 - 1] = L'\0';
    len = wcslen(buf);
    for (i = 0; i < len; i++) {
        if (buf[i] == L'\\' || buf[i] == L'/') {
            WCHAR keep = buf[i];
            buf[i] = L'\0';
            if (wcslen(buf) > 2) CreateDirectoryW(buf, NULL);
            buf[i] = keep;
        }
    }
    if (!CreateDirectoryW(buf, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) return 1;
    }
    return 0;
}

static int write_file(const WCHAR *path, const unsigned char *data, size_t len)
{
    HANDLE h;
    DWORD written = 0;
    WCHAR parent[MAX_PATH * 2];
    WCHAR *p;
    wcsncpy(parent, path, MAX_PATH * 2 - 1);
    parent[MAX_PATH * 2 - 1] = L'\0';
    p = wcsrchr(parent, L'\\');
    if (p) { *p = L'\0'; mkdir_deep(parent); }

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 1;
    if (len > 0 && !WriteFile(h, data, (DWORD)len, &written, NULL)) {
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);
    return written == len ? 0 : 1;
}

/* 读自身 exe 全部字节。 */
static unsigned char *read_self(size_t *out_len)
{
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD got = 0;
    unsigned char *buf;

    h = CreateFileW(g_exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)(256u * 1024u * 1024u)) {
        CloseHandle(h);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *out_len = got;
    return buf;
}

static const unsigned char *find_marker(const unsigned char *buf, size_t len, size_t *off)
{
    size_t i;
    if (len < MARKER_LEN) return NULL;
    for (i = len - MARKER_LEN + 1; i-- > 0;) {
        if (memcmp(buf + i, MARKER, MARKER_LEN) == 0) {
            *off = i + MARKER_LEN;
            return buf + i;
        }
    }
    return NULL;
}

/* 创建开始菜单快捷方式；失败只提示，不阻断安装。 */
static int make_shortcut(const WCHAR *install_dir)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    WCHAR lnk[MAX_PATH * 2];
    WCHAR target[MAX_PATH * 2];
    WCHAR programs[MAX_PATH];
    HRESULT hr;

    if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, programs) != S_OK) {
        setup_log("SHGetFolderPathW(CSIDL_PROGRAMS) failed");
        return 1;
    }
    setup_log("programs=%ls", programs);
    _snwprintf(lnk, MAX_PATH * 2 - 1, L"%s\\%s", programs, L"DSC");
    lnk[MAX_PATH * 2 - 1] = L'\0';
    CreateDirectoryW(lnk, NULL);
    {
        WCHAR name[MAX_PATH * 2];
        _snwprintf(name, MAX_PATH * 2 - 1, L"%s\\%s.lnk", lnk, L"DSC");
        name[MAX_PATH * 2 - 1] = L'\0';
        wcsncpy(lnk, name, MAX_PATH * 2 - 1);
        lnk[MAX_PATH * 2 - 1] = L'\0';
    }
    _snwprintf(target, MAX_PATH * 2 - 1, L"%s\\DSC.exe", install_dir);
    target[MAX_PATH * 2 - 1] = L'\0';

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    setup_log("CoCreateInstance hr=0x%08lx link=%p target=%ls", (unsigned long)hr, (void *)link, target);
    if (FAILED(hr) || !link) return 1;
    link->lpVtbl->SetPath(link, target);
    link->lpVtbl->SetWorkingDirectory(link, install_dir);
    link->lpVtbl->SetDescription(link, APP_NAME);
    hr = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr) && file) {
        hr = file->lpVtbl->Save(file, lnk, TRUE);
        file->lpVtbl->Release(file);
    }
    link->lpVtbl->Release(link);
    return FAILED(hr) ? 1 : 0;
}

static int write_uninstall_entry(const WCHAR *install_dir)
{
    HKEY key = NULL;
    WCHAR uninst[MAX_PATH * 2];
    DWORD dispos = 0;
    {
        LONG rv = RegCreateKeyExW(HKEY_CURRENT_USER, UNINST_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, &dispos);
        setup_log("RegCreateKeyExW rv=%ld", (long)rv);
        if (rv != ERROR_SUCCESS) return 1;
    }
    _snwprintf(uninst, MAX_PATH * 2 - 1, L"\"%s\\uninstall.exe\"", install_dir);
    uninst[MAX_PATH * 2 - 1] = L'\0';
    RegSetValueExW(key, L"DisplayName", 0, REG_SZ, (const BYTE *)APP_NAME,
                   (DWORD)((wcslen(APP_NAME) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, (const BYTE *)APP_VERSION,
                   (DWORD)((wcslen(APP_VERSION) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"DisplayIcon", 0, REG_SZ, (const BYTE *)uninst,
                   (DWORD)((wcslen(uninst) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"Publisher", 0, REG_SZ, (const BYTE *)APP_NAME,
                   (DWORD)((wcslen(APP_NAME) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"InstallLocation", 0, REG_SZ, (const BYTE *)install_dir,
                   (DWORD)((wcslen(install_dir) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"UninstallString", 0, REG_SZ, (const BYTE *)uninst,
                   (DWORD)((wcslen(uninst) + 1) * sizeof(WCHAR)));
    {
        DWORD one = 1;
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
        RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    }
    RegCloseKey(key);
    return 0;
}

static void default_install_dir(void)
{
    WCHAR base[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base) != S_OK) {
        GetTempPathW(MAX_PATH, base);
    }
    _snwprintf(g_dir, MAX_PATH * 2 - 1, L"%s\\Programs\\DSC", base);
    g_dir[MAX_PATH * 2 - 1] = L'\0';
}

static void parse_args(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int i;
    if (!argv) return;
    for (i = 1; i < argc; i++) {
        if (lstrcmpiW(argv[i], L"/S") == 0) g_silent = 1;
        else if (wcsncmp(argv[i], L"/D=", 3) == 0) {
            wcsncpy(g_dir, argv[i] + 3, MAX_PATH * 2 - 1);
            g_dir[MAX_PATH * 2 - 1] = L'\0';
        }
    }
    LocalFree(argv);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    unsigned char *buf = NULL;
    size_t len = 0, off = 0;
    const unsigned char *p, *end;
    uint32_t count = 0, i;
    int done = 0, failed = 0;
    WCHAR text[1024];

    (void)inst; (void)prev; (void)cmd; (void)show;

    GetModuleFileNameW(NULL, g_exe, MAX_PATH * 2);
    default_install_dir();
    parse_args();

    if (!g_silent && MessageBoxW(NULL,
            L"将把 DSC安装到下面这个目录：\n\n（可在命令行用 /D=<目录> 指定）\n\n继续安装？",
            APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return 0;
    }

    buf = read_self(&len);
    if (!buf) { msg(L"读取安装包失败。", MB_ICONERROR); return 2; }
    p = find_marker(buf, len, &off);
    if (!p) { free(buf); msg(L"安装包内容不完整（缺少 payload）。", MB_ICONERROR); return 3; }

    p = buf + off;
    end = buf + len;
    if (end - p < 4) { free(buf); msg(L"payload 头部损坏。", MB_ICONERROR); return 3; }
    memcpy(&count, p, 4);
    p += 4;

    /* 末尾追加解包步骤：逐个写出文件。 */
    for (i = 0; i < count; i++) {
        uint32_t plen = 0;
        uint64_t size = 0;
        WCHAR wpath[MAX_PATH * 2];
        WCHAR full[MAX_PATH * 2];
        char rel[1024];

        if (end - p < 4) { failed = 1; break; }
        memcpy(&plen, p, 4);
        p += 4;
        if (plen == 0 || plen >= (uint32_t)sizeof(rel) || (size_t)(end - p) < (size_t)plen) { failed = 1; break; }
        memcpy(rel, p, plen);
        rel[plen] = '\0';
        p += plen;
        if (end - p < 8) { failed = 1; break; }
        memcpy(&size, p, 8);
        p += 8;
        if (end - p < (ptrdiff_t)size) { failed = 1; break; }

        u8_to_w(rel, wpath, MAX_PATH * 2);
        {
            /* 归档里的分隔符统一为 /，落盘前换成 \ 以便逐层建目录。 */
            WCHAR *q = wpath;
            for (; *q; q++) if (*q == L'/') *q = L'\\';
        }
        _snwprintf(full, MAX_PATH * 2 - 1, L"%s\\%s", g_dir, wpath);
        full[MAX_PATH * 2 - 1] = L'\0';
        if (write_file(full, p, (size_t)size) != 0) { failed = 1; break; }
        p += (size_t)size;
        done++;
    }
    free(buf);

    if (failed) {
        _snwprintf(text, 1023, L"安装失败：已写入 %d/%u 个文件。请检查磁盘空间与目录权限后重试。",
                   done, (unsigned)count);
        text[1023] = L'\0';
        msg(text, MB_ICONERROR);
        return 4;
    }

    {
        WCHAR me[256];
        DWORD n = 256;
        if (GetUserNameW(me, &n)) setup_log("user=%ls", me);
        setup_log("payload ok: files=%d dir=%ls", done, g_dir);
    }
    {
        int sc, rg;
        WCHAR logpath[MAX_PATH * 2];

        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        sc = make_shortcut(g_dir);
        rg = write_uninstall_entry(g_dir);
        CoUninitialize();

        /* 把安装结果写进安装目录，便于用户与支持人员核对（失败不影响使用）。 */
        setup_log("shortcut rc=%d registry rc=%d", sc, rg);
        _snwprintf(logpath, MAX_PATH * 2 - 1, L"%s\\install.log", g_dir);
        logpath[MAX_PATH * 2 - 1] = L'\0';
        {
            char body[256];
            SYSTEMTIME st;
            int n;
            GetLocalTime(&st);
            n = snprintf(body, sizeof(body),
                         "DSC Setup 0.1.4\ntime=%04u-%02u-%02u %02u:%02u:%02u\nfiles=%d shortcut=%s registry=%s\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         done, sc == 0 ? "ok" : "failed", rg == 0 ? "ok" : "failed");
            if (n > 0) write_file(logpath, (const unsigned char *)body, (size_t)n);
        }

        if (!g_silent) {
            const WCHAR *sc_txt = sc == 0 ? L"已创建" : L"创建失败（可直接运行安装目录里的 DSC.exe）";
            const WCHAR *rg_txt = rg == 0 ? L"已登记（可在「设置 - 应用」卸载）" : L"登记失败（可手工删除安装目录）";
            _snwprintf(text, 1023, L"安装完成：%s\n\n开始菜单快捷方式：%s\n卸载入口：%s",
                       g_dir, sc_txt, rg_txt);
            text[1023] = L'\0';
            msg(text, (sc == 0 && rg == 0) ? MB_ICONINFORMATION : MB_ICONWARNING);
            {
                WCHAR target[MAX_PATH * 2];
                _snwprintf(target, MAX_PATH * 2 - 1, L"%s\\DSC.exe", g_dir);
                target[MAX_PATH * 2 - 1] = L'\0';
                ShellExecuteW(NULL, L"open", target, NULL, g_dir, SW_SHOWNORMAL);
            }
        }
    }
    return 0;
}
