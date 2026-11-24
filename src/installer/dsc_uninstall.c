/* dsc_uninstall.c —— DSC 卸载程序（纯 C，Win32）。
 *
 * 由安装程序落地到安装目录，并被登记为「设置 - 应用」里的卸载入口。
 * 行为：删除开始菜单快捷方式、清除卸载登记、删除安装目录；
 *       自身 exe 无法在运行中删除，最后交给 cmd 在进程退出后再删。
 *       /K 只清登记不删目录（保留数据用）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#define APP_NAME L"DSC"
#define UNINST_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DSC"

static int g_keep = 0;
static int g_silent = 0;
static WCHAR g_dir[MAX_PATH * 2];

static int remove_tree(const WCHAR *dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pattern[MAX_PATH * 2];
    int rc = 0;

    _snwprintf(pattern, MAX_PATH * 2 - 1, L"%s\\*", dir);
    pattern[MAX_PATH * 2 - 1] = L'\0';
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR full[MAX_PATH * 2];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        _snwprintf(full, MAX_PATH * 2 - 1, L"%s\\%s", dir, fd.cFileName);
        full[MAX_PATH * 2 - 1] = L'\0';
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_tree(full);
            if (!RemoveDirectoryW(full)) rc = 1;
        } else {
            SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full)) rc = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

static void remove_shortcut(void)
{
    WCHAR programs[MAX_PATH];
    WCHAR lnk[MAX_PATH * 2];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, programs) != S_OK) return;
    _snwprintf(lnk, MAX_PATH * 2 - 1, L"%s\\DSC\\DSC.lnk", programs);
    lnk[MAX_PATH * 2 - 1] = L'\0';
    DeleteFileW(lnk);
    _snwprintf(lnk, MAX_PATH * 2 - 1, L"%s\\DSC", programs);
    lnk[MAX_PATH * 2 - 1] = L'\0';
    RemoveDirectoryW(lnk);
}

/* 退出后再删自己：cmd 延迟 1 秒，rd /s /q 整个目录。 */
static void schedule_self_delete(void)
{
    WCHAR cmdline[MAX_PATH * 6];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    _snwprintf(cmdline, MAX_PATH * 6 - 1,
               L"cmd.exe /c ping 127.0.0.1 -n 2 >nul & rd /s /q \"%s\"", g_dir);
    cmdline[MAX_PATH * 6 - 1] = L'\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    int argc = 0;
    LPWSTR *argv;
    WCHAR *p;
    (void)inst; (void)prev; (void)cmd; (void)show;

    GetModuleFileNameW(NULL, g_dir, MAX_PATH * 2);
    p = wcsrchr(g_dir, L'\\');
    if (p) *p = L'\0';

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int i;
        for (i = 1; i < argc; i++) {
            if (lstrcmpiW(argv[i], L"/K") == 0) g_keep = 1;
            else if (lstrcmpiW(argv[i], L"/S") == 0) g_silent = 1;
        }
        LocalFree(argv);
    }

    if (!g_keep && !g_silent) {
        if (MessageBoxW(NULL, L"要卸载 DSC吗？\n\n会话记录与配置保存在用户数据目录，卸载默认保留。",
                        APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return 0;
        }
    }

    remove_shortcut();
    RegDeleteKeyW(HKEY_CURRENT_USER, UNINST_KEY);

    if (!g_keep) {
        /* 删除除自身以外的所有文件，随后由 cmd 删除剩余目录。 */
        WCHAR self[MAX_PATH * 2];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        WCHAR pattern[MAX_PATH * 2];

        GetModuleFileNameW(NULL, self, MAX_PATH * 2);
        _snwprintf(pattern, MAX_PATH * 2 - 1, L"%s\\*", g_dir);
        pattern[MAX_PATH * 2 - 1] = L'\0';
        h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                WCHAR full[MAX_PATH * 2];
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                _snwprintf(full, MAX_PATH * 2 - 1, L"%s\\%s", g_dir, fd.cFileName);
                full[MAX_PATH * 2 - 1] = L'\0';
                if (lstrcmpiW(full, self) == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    remove_tree(full);
                    RemoveDirectoryW(full);
                } else {
                    SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(full);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        schedule_self_delete();
    }

    if (!g_silent) {
        if (!g_keep) MessageBoxW(NULL, L"已卸载。用户数据目录未删除，可手工清理。", APP_NAME, MB_ICONINFORMATION);
        else MessageBoxW(NULL, L"已清除系统登记，安装目录与用户数据保留。", APP_NAME, MB_ICONINFORMATION);
    }
    return 0;
}
