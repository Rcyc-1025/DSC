/* http_win.c —— Windows 侧 HTTP 实现（WinHTTP，系统组件）。
 *
 * 只做三件事：流式 POST（把响应字节直接喂给 aic_sse_feed）、断点续传下载、GET 小 JSON。
 * 代理、TLS、重定向都由 WinHTTP 负责，不引入任何第三方网络库。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diaglog.h"
#include "http.h"
#include "llm.h"
#include "util.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

typedef struct win_url {
    wchar_t       host[256];
    wchar_t       path[1024];
    INTERNET_PORT port;
    int           https;
} win_url;

static void widen(const char *s, wchar_t *out, int cap)
{
    if (!s) { if (cap > 0) out[0] = 0; return; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
}

static int split_url(const char *url, win_url *out)
{
    wchar_t w[AIC_LLM_URL_MAX];
    memset(out, 0, sizeof(*out));
    widen(url, w, (int)(sizeof(w) / sizeof(w[0])));
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w, 0, 0, &uc)) return AIC_E_INVAL;
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) return AIC_E_INVAL;
    wcsncpy(out->host, uc.lpszHostName, uc.dwHostNameLength);
    out->host[uc.dwHostNameLength] = 0;
    size_t n = 0;
    size_t cap = sizeof(out->path) / sizeof(out->path[0]);
    if (uc.lpszUrlPath && uc.dwUrlPathLength > 0) {
        size_t len = uc.dwUrlPathLength;
        if (len >= cap) len = cap - 1;
        wcsncpy(out->path, uc.lpszUrlPath, len);
        n = len;
    } else {
        out->path[n++] = L'/';
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) {
        size_t len = uc.dwExtraInfoLength;
        if (n + len >= cap) len = cap - 1 - n;
        wcsncpy(out->path + n, uc.lpszExtraInfo, len);
        n += len;
    }
    out->path[n] = 0;
    out->port = uc.nPort;
    out->https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return AIC_OK;
}

static HINTERNET open_session(void)
{
    HINTERNET h = WinHttpOpen(L"Sai/1.0 (Windows; pure C)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) {
        h = WinHttpOpen(L"Sai/1.0 (Windows; pure C)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!h) return NULL;
    WinHttpSetTimeouts(h, 15000, 15000, 60000, 120000);
    return h;
}

static DWORD query_status(HINTERNET req)
{
    DWORD status = 0;
    DWORD len = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return status;
}

/* 读完整响应体（错误场景下拿可读信息，最多 8KB）。 */
static void read_body_text(HINTERNET req, char *out, size_t cap)
{
    size_t used = 0;
    DWORD got = 0;
    char buf[2048];
    while (used + 1 < cap && WinHttpReadData(req, buf, sizeof(buf) - 1, &got) && got > 0) {
        size_t take = got;
        if (take > cap - 1 - used) take = cap - 1 - used;
        memcpy(out + used, buf, take);
        used += take;
        if (take < got) break;
    }
    out[used] = '\0';
    for (size_t i = 0; i < used; i++) if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
}

static int cancelled(volatile int *cancel)
{
    return cancel && *cancel;
}

static aic_llm_err win_post(void *ud, const char *url, const char *key, const char *body,
                            aic_sse_parser *parser, aic_llm_event_fn fn, void *cb_ud,
                            volatile int *cancel, char *err, size_t errlen)
{
    (void)ud;
    if (err && errlen) err[0] = '\0';
    win_url u;
    if (split_url(url, &u) != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "请求地址无效，请检查设置里的接口地址");
        return AIC_LLM_E_NETWORK;
    }
    HINTERNET ses = open_session();
    if (!ses) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法初始化网络组件（WinHTTP）");
        return AIC_LLM_E_NETWORK;
    }
    HINTERNET con = WinHttpConnect(ses, u.host, u.port, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        if (err && errlen) snprintf(err, errlen, "无法连接服务器（错误码 %lu）", (unsigned long)GetLastError());
        return AIC_LLM_E_NETWORK;
    }
    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"POST", u.path, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法创建请求（WinHTTP）");
        return AIC_LLM_E_NETWORK;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    wchar_t headers[1024];
    wchar_t wkey[512];
    widen(key ? key : "", wkey, (int)(sizeof(wkey) / sizeof(wkey[0])));
    _snwprintf(headers, (sizeof(headers) / sizeof(wchar_t)) - 1,
               L"Content-Type: application/json; charset=utf-8\r\n"
               L"Accept: text/event-stream\r\n"
               L"Authorization: Bearer %ls\r\n",
               wkey);
    headers[(sizeof(headers) / sizeof(wchar_t)) - 1] = 0;

    size_t body_len = body ? strlen(body) : 0;
    BOOL ok = WinHttpSendRequest(req, headers, (DWORD)-1L, (LPVOID)body, (DWORD)body_len,
                                 (DWORD)body_len, 0);
    if (!ok || !WinHttpReceiveResponse(req, NULL)) {
        DWORD e = GetLastError();
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        if (cancelled(cancel)) {
            if (err && errlen) aic_strlcpy_utf8(err, errlen, "已停止生成");
            return AIC_LLM_E_CANCEL;
        }
        if (err && errlen) snprintf(err, errlen, "网络请求失败（WinHTTP 错误 %lu）", (unsigned long)e);
        aic_log_write("WARN", "WinHTTP 请求失败：url=%s err=%lu", url, (unsigned long)e);
        return AIC_LLM_E_NETWORK;
    }

    DWORD status = query_status(req);
    if (status != 200) {
        char snippet[1024];
        read_body_text(req, snippet, sizeof(snippet));
        aic_log_write("WARN", "模型接口返回 HTTP %lu", (unsigned long)status);
        aic_log_blob("模型接口错误响应", snippet, strlen(snippet));
        aic_llm_err cls = aic_llm_classify_http((int)status);
        if (err && errlen) {
            snprintf(err, errlen, "%s（HTTP %lu）%s%s", aic_llm_err_text(cls),
                     (unsigned long)status, snippet[0] ? "：" : "", snippet);
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return cls;
    }

    /* 流式读：每块直接进 SSE 解析器，界面就能边收边显示。 */
    char buf[4096];
    DWORD got = 0;
    aic_llm_err result = AIC_LLM_OK;
    for (;;) {
        if (cancelled(cancel)) { result = AIC_LLM_E_CANCEL; break; }
        if (!WinHttpReadData(req, buf, sizeof(buf), &got)) {
            if (cancelled(cancel)) { result = AIC_LLM_E_CANCEL; break; }
            DWORD e = GetLastError();
            if (err && errlen) snprintf(err, errlen, "读取响应失败（WinHTTP 错误 %lu）", (unsigned long)e);
            aic_log_write("WARN", "WinHTTP 读取失败：%lu", (unsigned long)e);
            result = AIC_LLM_E_NETWORK;
            break;
        }
        if (got == 0) break;
        if (aic_sse_feed(parser, buf, got, fn, cb_ud) != AIC_OK) {
            result = AIC_LLM_E_PARSE;
            break;
        }
    }
    if (result == AIC_LLM_E_CANCEL && err && errlen) aic_strlcpy_utf8(err, errlen, "已停止生成");
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return result;
}

static aic_llm_err win_probe(void *ud, const char *url, const char *key, char *err, size_t errlen)
{
    (void)ud;
    if (err && errlen) err[0] = '\0';
    win_url u;
    if (split_url(url, &u) != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "请求地址无效");
        return AIC_LLM_E_NETWORK;
    }
    HINTERNET ses = open_session();
    if (!ses) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法初始化网络组件（WinHTTP）");
        return AIC_LLM_E_NETWORK;
    }
    HINTERNET con = WinHttpConnect(ses, u.host, u.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/models", NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             u.https ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!req) {
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法创建连通性校验请求");
        return AIC_LLM_E_NETWORK;
    }
    wchar_t headers[1024];
    wchar_t wkey[512];
    widen(key ? key : "", wkey, (int)(sizeof(wkey) / sizeof(wkey[0])));
    _snwprintf(headers, (sizeof(headers) / sizeof(wchar_t)) - 1,
               L"Accept: application/json\r\nAuthorization: Bearer %ls\r\n", wkey);
    headers[(sizeof(headers) / sizeof(wchar_t)) - 1] = 0;

    aic_llm_err result = AIC_LLM_OK;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        if (err && errlen) snprintf(err, errlen, "连通性校验失败（错误码 %lu）", (unsigned long)GetLastError());
        result = AIC_LLM_E_NETWORK;
    } else {
        DWORD status = query_status(req);
        if (status != 200) {
            char snippet[512];
            read_body_text(req, snippet, sizeof(snippet));
            result = aic_llm_classify_http((int)status);
            if (err && errlen) snprintf(err, errlen, "%s（HTTP %lu）", aic_llm_err_text(result),
                                        (unsigned long)status);
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return result;
}

int aic_http_available(void) { return 1; }

int aic_http_get_json(const char *url, const char *api_key, char **out_body,
                      int *http_status, char *err, size_t errlen)
{
    if (!url || !out_body) return AIC_E_INVAL;
    *out_body = NULL;
    if (http_status) *http_status = 0;
    if (err && errlen) err[0] = '\0';
    win_url u;
    if (split_url(url, &u) != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "地址无效");
        return AIC_E_INVAL;
    }
    HINTERNET ses = open_session();
    if (!ses) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法初始化网络组件（WinHTTP）");
        return AIC_E_TIMEOUT;
    }
    HINTERNET con = WinHttpConnect(ses, u.host, u.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", u.path, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             u.https ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!req) {
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法创建请求");
        return AIC_E_TIMEOUT;
    }
    wchar_t headers[1024];
    wchar_t wkey[512];
    widen(api_key ? api_key : "", wkey, (int)(sizeof(wkey) / sizeof(wkey[0])));
    _snwprintf(headers, (sizeof(headers) / sizeof(wchar_t)) - 1,
               L"Accept: application/json\r\nAuthorization: Bearer %ls\r\n", wkey);
    headers[(sizeof(headers) / sizeof(wchar_t)) - 1] = 0;

    int rc = AIC_OK;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        if (err && errlen) snprintf(err, errlen, "查询失败（错误码 %lu）", (unsigned long)GetLastError());
        rc = AIC_E_TIMEOUT;
    } else {
        DWORD status = query_status(req);
        if (http_status) *http_status = (int)status;
        sb body;
        sb_init(&body);
        char buf[2048];
        DWORD got = 0;
        while (WinHttpReadData(req, buf, sizeof(buf) - 1, &got) && got > 0) {
            sb_appendn(&body, buf, got);
            if (strlen(sb_cstr(&body)) > 256 * 1024) break;
        }
        *out_body = aic_strdup(sb_cstr(&body));
        sb_free(&body);
        if (!*out_body) rc = AIC_E_NOMEM;
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return rc;
}

int aic_http_download(const char *url, const char *dst, long long max_bytes, int resume,
                      aic_http_progress_fn prog, void *prog_ud, volatile int *cancel,
                      char *err, size_t errlen)
{
    if (!url || !dst) return AIC_E_INVAL;
    if (err && errlen) err[0] = '\0';
    win_url u;
    if (split_url(url, &u) != AIC_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "下载地址无效");
        return AIC_E_INVAL;
    }
    HINTERNET ses = open_session();
    if (!ses) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法初始化网络组件（WinHTTP）");
        return AIC_E_TIMEOUT;
    }
    HINTERNET con = WinHttpConnect(ses, u.host, u.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", u.path, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             u.https ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!req) {
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "无法创建下载请求");
        return AIC_E_TIMEOUT;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    long long have = 0;
    if (resume) {
        int64_t sz = 0;
        if (aic_file_size(dst, &sz) == AIC_OK && sz > 0) have = (long long)sz;
    }
    wchar_t headers[512];
    headers[0] = 0;
    if (have > 0) {
        _snwprintf(headers, (sizeof(headers) / sizeof(wchar_t)) - 1,
                   L"Range: bytes=%lld-\r\n", have);
        headers[(sizeof(headers) / sizeof(wchar_t)) - 1] = 0;
    }

    int rc = AIC_OK;
    FILE *fp = NULL;
    long long got = have;
    if (!WinHttpSendRequest(req, have > 0 ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                            have > 0 ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        if (err && errlen) snprintf(err, errlen, "下载失败（错误码 %lu）", (unsigned long)GetLastError());
        rc = AIC_E_TIMEOUT;
        goto done;
    }
    {
        DWORD status = query_status(req);
        int append = 0;
        if (status == 206 && have > 0) {
            append = 1;
        } else if (status == 200) {
            append = 0;
            got = 0;
        } else {
            if (err && errlen) snprintf(err, errlen, "下载失败（HTTP %lu）", (unsigned long)status);
            rc = AIC_E_STATE;
            goto done;
        }
        fp = fopen(dst, append ? "ab" : "wb");
        if (!fp) {
            if (err && errlen) snprintf(err, errlen, "无法写入下载文件：%s", dst);
            rc = AIC_E_IO;
            goto done;
        }
        char buf[8192];
        DWORD n = 0;
        for (;;) {
            if (cancelled(cancel)) { rc = AIC_E_STATE; if (err && errlen) aic_strlcpy_utf8(err, errlen, "下载已取消"); break; }
            if (!WinHttpReadData(req, buf, sizeof(buf), &n)) {
                if (err && errlen) snprintf(err, errlen, "下载中断（错误码 %lu）", (unsigned long)GetLastError());
                rc = AIC_E_TIMEOUT;
                break;
            }
            if (n == 0) break;
            if (max_bytes > 0 && got + (long long)n > max_bytes) {
                if (err && errlen) snprintf(err, errlen, "安装包超过大小上限（%lld 字节）", max_bytes);
                rc = AIC_E_LIMIT;
                break;
            }
            if (fwrite(buf, 1, n, fp) != n) {
                if (err && errlen) aic_strlcpy_utf8(err, errlen, "写入下载文件失败（磁盘空间不足？）");
                rc = AIC_E_IO;
                break;
            }
            got += (long long)n;
            if (prog) prog(prog_ud, got, max_bytes > 0 ? max_bytes : 0);
        }
    }
done:
    if (fp) fclose(fp);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return rc;
}

aic_llm_transport aic_win_http_transport(void)
{
    aic_llm_transport t;
    memset(&t, 0, sizeof(t));
    t.ud = NULL;
    t.post_stream = win_post;
    t.probe = win_probe;
    return t;
}
