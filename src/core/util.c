/* util.c —— 基础工具实现（纯 C11，Windows / POSIX 双实现）。 */
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  include <sys/stat.h>
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

/* 错误文案与枚举名实现在 aic_types.c 中，此处不再重复定义。 */

/* ================= 内存 ================= */
void *aic_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        /* 无可恢复路径：直接失败，避免返回 NULL 被误用。 */
        abort();
    }
    return p;
}

void *aic_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) abort();
    return p;
}

void *aic_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) abort();
    return q;
}

void aic_xfree(void *p)
{
    if (p) free(p);
}

char *aic_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)aic_xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

char *aic_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *d = (char *)aic_xmalloc(len + 1);
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

void aic_strlcpy_utf8(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n + 1 <= cap) {
        memcpy(dst, src, n + 1);
        return;
    }
    size_t keep = cap - 1;
    /* 回退到 UTF-8 字符边界：不要留下半个多字节字符。 */
    while (keep > 0 && ((unsigned char)src[keep] & 0xC0) == 0x80) keep--;
    memcpy(dst, src, keep);
    dst[keep] = '\0';
}

/* ================= 字符串缓冲 ================= */
void sb_init(sb *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(sb *b)
{
    if (!b) return;
    aic_xfree(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void sb_reset(sb *b)
{
    if (!b) return;
    b->len = 0;
    if (b->data && b->cap) b->data[0] = '\0';
}

int sb_reserve(sb *b, size_t need)
{
    if (b->cap >= need + 1) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need + 1) cap *= 2;
    b->data = (char *)aic_xrealloc(b->data, cap);
    b->cap = cap;
    return 0;
}

int sb_appendn(sb *b, const char *s, size_t n)
{
    if (!s || n == 0) {
        sb_reserve(b, b->len);
        b->data[b->len] = '\0';
        return 0;
    }
    sb_reserve(b, b->len + n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int sb_append(sb *b, const char *s)
{
    return s ? sb_appendn(b, s, strlen(s)) : sb_appendn(b, "", 0);
}

int sb_appendc(sb *b, char c)
{
    sb_reserve(b, b->len + 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

int sb_appendf(sb *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { va_end(ap2); return -1; }
    sb_reserve(b, b->len + (size_t)need);
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
    return 0;
}

const char *sb_cstr(sb *b)
{
    if (!b->data) {
        sb_reserve(b, 0);
        b->data[0] = '\0';
    }
    return b->data;
}

/* ================= UTF-8 ================= */
size_t aic_utf8_length(const char *s)
{
    if (!s) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

int aic_utf8_valid(const char *s)
{
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        size_t extra;
        if (c < 0x80) { p++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; if (c < 0xC2) return 0; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; if (c > 0xF4) return 0; }
        else return 0;
        p++;
        for (size_t i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) return 0;
            p++;
        }
    }
    return 1;
}

size_t aic_utf8_truncate_chars(char *s, size_t max_chars)
{
    if (!s) return 0;
    size_t chars = 0, i = 0;
    while (s[i]) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (chars == max_chars) { s[i] = '\0'; return i; }
            chars++;
        }
        i++;
    }
    return i;
}

/* ================= 时间 ================= */
int64_t aic_now_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* FILETIME 起点 1601-01-01，单位 100ns。 */
    return (int64_t)(t / 10000ull) - 11644473600000ll;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
#endif
}

void aic_ms_to_iso8601(int64_t ms, char buf[32])
{
    time_t secs = (time_t)(ms / 1000);
    int msec = (int)(ms % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &secs);
#else
    gmtime_r(&secs, &tmv);
#endif
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msec);
    aic_strlcpy_utf8(buf, 32, tmp);
}

void aic_now_iso8601(char buf[32])
{
    aic_ms_to_iso8601(aic_now_ms(), buf);
}

int aic_parse_iso8601(const char *s, int64_t *out_ms)
{
    if (!s || !out_ms) return AIC_E_INVAL;
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0, ms = 0;
    int n = sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &se);
    if (n < 3) return AIC_E_FORMAT;
    /* 可选毫秒与本地时区偏移。 */
    const char *p = s;
    while (*p && *p != 'T' && *p != ' ') p++;
    if (*p == 'T' || *p == ' ') {
        p++;
        while (*p && *p != '.' && *p != 'Z' && *p != '+' && *p != '-') p++;
    } else {
        p = "";
    }
    int offset_min = 0;
    if (*p == '.') {
        p++;
        int mult = 100;
        while (*p >= '0' && *p <= '9' && mult > 0) {
            ms += (*p - '0') * mult;
            mult /= 10;
            p++;
        }
    }
    if (*p == '+' || *p == '-') {
        int sign = (*p == '-') ? -1 : 1;
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) == 2) offset_min = sign * (oh * 60 + om);
    }
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
#ifdef _WIN32
    time_t t = _mkgmtime(&tmv);
#else
    time_t t = timegm(&tmv);
#endif
    if (t == (time_t)-1) return AIC_E_FORMAT;
    *out_ms = (int64_t)t * 1000ll + ms - (int64_t)offset_min * 60000ll;
    return AIC_OK;
}

int64_t aic_iso8601_to_ms(const char *s)
{
    int64_t ms = 0;
    if (aic_parse_iso8601(s, &ms) != AIC_OK) return 0;
    return ms;
}

/* ================= 随机源 ================= */
int aic_random_bytes(void *buf, size_t n)
{
    if (!buf || n == 0) return AIC_E_INVAL;
#ifdef _WIN32
    /* advapi32 导出的 SystemFunction036（即 RtlGenRandom），无需额外链接 bcrypt。 */
    typedef unsigned char(__stdcall *rtl_gen_random_fn)(void *, unsigned long);
    static rtl_gen_random_fn fn = NULL;
    if (!fn) {
        HMODULE h = LoadLibraryA("advapi32.dll");
        if (h) fn = (rtl_gen_random_fn)(void *)GetProcAddress(h, "SystemFunction036");
    }
    if (fn && fn(buf, (unsigned long)n)) return AIC_OK;
    return AIC_E_IO;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return AIC_E_IO;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    if (got != n) return AIC_E_IO;
    return AIC_OK;
#endif
}

/* ================= UUID ================= */
static uint64_t g_uuid_counter = 0;

void aic_uuid4(char out[37])
{
    uint8_t b[16];
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        unsigned long long seed = (unsigned long long)aic_now_ms();
#ifdef _WIN32
        seed ^= ((unsigned long long)GetCurrentProcessId() << 32);
#endif
        srand((unsigned)seed);
    }
    uint64_t ctr = ++g_uuid_counter;
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(rand() & 0xFF);
    /* 混入计数器与时间，降低同毫秒碰撞概率。 */
    for (int i = 0; i < 8; i++) b[i] ^= (uint8_t)((ctr >> (i * 8)) & 0xFF);
    uint64_t now = (uint64_t)aic_now_ms();
    for (int i = 8; i < 16; i++) b[i] ^= (uint8_t)((now >> ((i - 8) * 8)) & 0xFF);
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); /* variant */
    static const char *hex = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[pos++] = '-';
        out[pos++] = hex[b[i] >> 4];
        out[pos++] = hex[b[i] & 0x0F];
    }
    out[pos] = '\0';
}

int aic_uuid_valid(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != 36) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else if (!isxdigit((unsigned char)c)) {
            return 0;
        }
    }
    return 1;
}

/* ================= SHA-256 ================= */
static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

void aic_sha256_init(aic_sha256_ctx *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

static void sha256_block(aic_sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void aic_sha256_update(aic_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8ull;
    if (c->buflen) {
        size_t need = 64 - c->buflen;
        size_t take = len < need ? len : need;
        memcpy(c->buffer + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buffer);
            c->buflen = 0;
        }
    }
    while (len >= 64) {
        sha256_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buffer, p, len);
        c->buflen = len;
    }
}

void aic_sha256_final(aic_sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->bitlen;
    uint8_t pad = 0x80;
    aic_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) aic_sha256_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)((bits >> (56 - i * 8)) & 0xFF);
    /* 直接喂入长度，避免影响 bitlen 语义。 */
    memcpy(c->buffer + 56, lenbuf, 8);
    sha256_block(c, c->buffer);
    c->buflen = 0;
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void aic_sha256(const void *data, size_t len, uint8_t out[32])
{
    aic_sha256_ctx c;
    aic_sha256_init(&c);
    aic_sha256_update(&c, data, len);
    aic_sha256_final(&c, out);
}

void aic_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int aic_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!hex) return AIC_E_INVAL;
    size_t n = strlen(hex);
    if (n % 2) return AIC_E_FORMAT;
    size_t bytes = n / 2;
    if (bytes > cap) return AIC_E_LIMIT;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return AIC_E_FORMAT;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    if (out_len) *out_len = bytes;
    return AIC_OK;
}

void aic_sha256_hex(const void *data, size_t len, char out[65])
{
    uint8_t d[32];
    aic_sha256(data, len, d);
    aic_hex_encode(d, 32, out);
}

/* ================= 路径与文件 ================= */
int aic_path_join(char *dst, size_t cap, const char *a, const char *b)
{
    if (!dst || cap == 0 || !a) return AIC_E_INVAL;
    if (!b || !b[0]) {
        snprintf(dst, cap, "%s", a);
        return AIC_OK;
    }
    size_t la = strlen(a);
    int need_sep = la > 0 && a[la - 1] != '/' && a[la - 1] != '\\';
    int extra = (b[0] == '/' || b[0] == '\\') ? 1 : 0;
    int n = snprintf(dst, cap, "%s%s%s", a, need_sep ? "/" : "", b + extra);
    if (n < 0 || (size_t)n >= cap) return AIC_E_LIMIT;
    return AIC_OK;
}

int aic_path_exists(const char *p)
{
    if (!p || !p[0]) return 0;
#ifdef _WIN32
    struct _stat st;
    return _stat(p, &st) == 0;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

int aic_is_dir(const char *p)
{
#ifdef _WIN32
    struct _stat st;
    if (_stat(p, &st) != 0) return 0;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
#endif
}

int aic_mkdir_p(const char *p)
{
    if (!p || !p[0]) return AIC_E_INVAL;
    char buf[1024];
    size_t n = strlen(p);
    if (n >= sizeof(buf)) return AIC_E_LIMIT;
    memcpy(buf, p, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char save = buf[i];
            buf[i] = '\0';
            /* 跳过盘符根（"C:"）。 */
            if (!(strlen(buf) == 2 && buf[1] == ':')) {
                if (!aic_is_dir(buf)) {
#ifdef _WIN32
                    _mkdir(buf);
#else
                    mkdir(buf, 0777);
#endif
                }
            }
            buf[i] = save;
        }
    }
    if (!aic_is_dir(buf)) {
#ifdef _WIN32
        _mkdir(buf);
#else
        mkdir(buf, 0777);
#endif
    }
    return aic_is_dir(buf) ? AIC_OK : AIC_E_IO;
}

int aic_file_size(const char *p, int64_t *out)
{
    if (!p || !out) return AIC_E_INVAL;
#ifdef _WIN32
    struct _stat st;
    if (_stat(p, &st) != 0) return AIC_E_IO;
#else
    struct stat st;
    if (stat(p, &st) != 0) return AIC_E_IO;
#endif
    *out = (int64_t)st.st_size;
    return AIC_OK;
}

int aic_file_read_all(const char *p, char **out, size_t *out_len)
{
    if (!p || !out) return AIC_E_INVAL;
    FILE *f = fopen(p, "rb");
    if (!f) return AIC_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return AIC_E_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return AIC_E_IO; }
    rewind(f);
    char *buf = (char *)aic_xmalloc((size_t)sz + 1);
    size_t rd = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    buf[rd] = '\0';
    *out = buf;
    if (out_len) *out_len = rd;
    return AIC_OK;
}

int aic_file_write_all(const char *p, const void *data, size_t len)
{
    if (!p) return AIC_E_INVAL;
    FILE *f = fopen(p, "wb");
    if (!f) return AIC_E_IO;
    size_t wr = len ? fwrite(data, 1, len, f) : 0;
    int rc = fclose(f);
    if (wr != len || rc != 0) return AIC_E_IO;
    return AIC_OK;
}

int aic_file_append(const char *p, const void *data, size_t len)
{
    if (!p) return AIC_E_INVAL;
    FILE *f = fopen(p, "ab");
    if (!f) return AIC_E_IO;
    size_t wr = len ? fwrite(data, 1, len, f) : 0;
    int rc = fclose(f);
    if (wr != len || rc != 0) return AIC_E_IO;
    return AIC_OK;
}

int aic_file_copy(const char *src, const char *dst)
{
    char *buf = NULL;
    size_t len = 0;
    int rc = aic_file_read_all(src, &buf, &len);
    if (rc != AIC_OK) return rc;
    rc = aic_file_write_all(dst, buf, len);
    aic_xfree(buf);
    return rc;
}

int aic_file_remove(const char *p)
{
    if (!p) return AIC_E_INVAL;
#ifdef _WIN32
    return _unlink(p) == 0 ? AIC_OK : AIC_E_IO;
#else
    return unlink(p) == 0 ? AIC_OK : AIC_E_IO;
#endif
}

int aic_file_move(const char *src, const char *dst)
{
    if (aic_file_copy(src, dst) != AIC_OK) return AIC_E_IO;
    aic_file_remove(src);
    return AIC_OK;
}

const char *aic_path_basename(const char *p)
{
    if (!p) return "";
    const char *last = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') last = q + 1;
    return last;
}

int aic_dir_list(const char *dir, sb *names_out, char sep)
{
    if (!dir || !names_out) return AIC_E_INVAL;
    int count = 0;
#ifdef _WIN32
    char pattern[1024];
    if (snprintf(pattern, sizeof(pattern), "%s/*", dir) >= (int)sizeof(pattern)) return AIC_E_LIMIT;
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return AIC_OK;
    do {
        if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
        if (names_out->len) sb_appendc(names_out, sep);
        sb_append(names_out, fd.name);
        count++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return AIC_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (names_out->len) sb_appendc(names_out, sep);
        sb_append(names_out, e->d_name);
        count++;
    }
    closedir(d);
#endif
    return count;
}

/* ================= 其他工具 ================= */
int aic_str_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int aic_parse_int(const char *s, int *out)
{
    if (!s || !out) return AIC_E_INVAL;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0' && !isspace((unsigned char)*end))) return AIC_E_FORMAT;
    *out = (int)v;
    return AIC_OK;
}

int aic_parse_ll(const char *s, long long *out)
{
    if (!s || !out) return AIC_E_INVAL;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s) return AIC_E_FORMAT;
    *out = v;
    return AIC_OK;
}

int aic_is_valid_date(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n == 0) return 1; /* 空值合法（未填写） */
    if (n != 10) return 0;
    if (s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    int Y = atoi(s), M = atoi(s + 5), D = atoi(s + 8);
    if (Y < 1900 || Y > 2200) return 0;
    if (M < 1 || M > 12) return 0;
    static const int dim[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxd = dim[M];
    if (M == 2) {
        int leap = (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0);
        if (leap) maxd = 29;
    }
    return D >= 1 && D <= maxd;
}

int aic_is_local_relative_path(const char *s)
{
    if (!s || !s[0]) return 0;
    if (s[0] == '/' || s[0] == '\\') return 0;
    if (strlen(s) > 1 && s[1] == ':') return 0;
    if (strstr(s, "://") != NULL) return 0;
    if (strstr(s, "..") != NULL) return 0;
    if (s[0] == '\0') return 0;
    /* 允许的中段字符：字母数字、下划线、点、连字符、斜杠、反斜杠、常见 UTF-8 字节。 */
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20) return 0;
        if (*p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') return 0;
    }
    return 1;
}

int aic_version_compare(const char *a, const char *b)
{
    if (!a) a = "0";
    if (!b) b = "0";
    for (;;) {
        long na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') { na = na * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { nb = nb * 10 + (*b - '0'); b++; }
        if (na != nb) return na < nb ? -1 : 1;
        while (*a == '.') a++;
        while (*b == '.') b++;
        if (!*a && !*b) return 0;
        /* 段数不同时，缺失的段按 0 处理：2.0 与 2.0.0 等价。 */
        int a_num = !*a || isdigit((unsigned char)*a);
        int b_num = !*b || isdigit((unsigned char)*b);
        if (!a_num || !b_num) {
            /* 非数字尾缀（如 -beta）按字典序比较。 */
            const char *ra = *a ? a : "";
            const char *rb = *b ? b : "";
            int c = strcmp(ra, rb);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
    }
}

int aic_hex_valid(const char *hex, size_t need_len)
{
    if (!hex) return 0;
    size_t n = strlen(hex);
    if (n != need_len) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = hex[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return 0;
    }
    return 1;
}

int aic_version_valid(const char *v)
{
    if (!v || !v[0]) return 0;
    int segments = 0;
    const char *p = v;
    for (;;) {
        const char *start = p;
        while (*p >= '0' && *p <= '9') p++;
        if (p == start) return 0;           /* 段内必须有数字 */
        if (p - start > 9) return 0;
        segments++;
        if (segments > 4) return 0;
        if (*p == '\0') return segments >= 2 ? 1 : 0;
        if (*p != '.') return 0;
        p++;
    }
}

int aic_scan_sensitive(const char *text, sb *hit)
{
    if (!text) return 0;
    struct pattern {
        const char *prefix;
        size_t min_tail;
        const char *label;
    };
    static const struct pattern pats[] = {
        { "sk-", 16, "疑似 DeepSeek/OpenAI 风格的 API Key" },
        { "gho_", 20, "疑似 GitHub OAuth Token" },
        { "ghp_", 20, "疑似 GitHub Personal Access Token" },
        { "AKIA", 16, "疑似 AWS Access Key" },
        { "-----BEGIN RSA PRIVATE KEY-----", 0, "疑似 RSA 私钥" },
        { "-----BEGIN PRIVATE KEY-----", 0, "疑似私钥" },
        { "-----BEGIN OPENSSH PRIVATE KEY-----", 0, "疑似 OpenSSH 私钥" }
    };
    int found = 0;
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        const char *p = text;
        while ((p = strstr(p, pats[i].prefix)) != NULL) {
            if (pats[i].min_tail == 0) {
                if (hit) sb_appendf(hit, "%s；", pats[i].label);
                found = 1;
                break;
            }
            size_t tail = 0;
            const char *q = p + strlen(pats[i].prefix);
            while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '-')) { tail++; q++; }
            if (tail >= pats[i].min_tail) {
                if (hit) sb_appendf(hit, "%s（前缀 %s，长度 %u）；", pats[i].label,
                                    pats[i].prefix, (unsigned)tail);
                found = 1;
                break;
            }
            p += strlen(pats[i].prefix);
        }
    }
    return found;
}

/* ---------- Base64 ---------- */
static const char g_b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int aic_base64_encode(const void *data, size_t len, sb *out)
{
    if (!out) return AIC_E_INVAL;
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        unsigned int v = ((unsigned int)p[i] << 16) | ((unsigned int)p[i + 1] << 8) | p[i + 2];
        sb_appendc(out, g_b64tab[(v >> 18) & 0x3F]);
        sb_appendc(out, g_b64tab[(v >> 12) & 0x3F]);
        sb_appendc(out, g_b64tab[(v >> 6) & 0x3F]);
        sb_appendc(out, g_b64tab[v & 0x3F]);
    }
    if (len - i == 1) {
        unsigned int v = (unsigned int)p[i] << 16;
        sb_appendc(out, g_b64tab[(v >> 18) & 0x3F]);
        sb_appendc(out, g_b64tab[(v >> 12) & 0x3F]);
        sb_appendc(out, '=');
        sb_appendc(out, '=');
    } else if (len - i == 2) {
        unsigned int v = ((unsigned int)p[i] << 16) | ((unsigned int)p[i + 1] << 8);
        sb_appendc(out, g_b64tab[(v >> 18) & 0x3F]);
        sb_appendc(out, g_b64tab[(v >> 12) & 0x3F]);
        sb_appendc(out, g_b64tab[(v >> 6) & 0x3F]);
        sb_appendc(out, '=');
    }
    return AIC_OK;
}

static int b64val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

int aic_base64_decode(const char *s, size_t len, unsigned char **out, size_t *out_len)
{
    if (!s || !out || !out_len) return AIC_E_INVAL;
    unsigned char *buf = (unsigned char *)aic_xmalloc(len / 4 * 3 + 4);
    size_t n = 0;
    int quad[4];
    int qn = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        if (ch == '=') {
            quad[qn++] = 0;
            if (qn == 4) {
                unsigned int v = ((unsigned int)quad[0] << 18) | ((unsigned int)quad[1] << 12) |
                                 ((unsigned int)quad[2] << 6) | (unsigned int)quad[3];
                buf[n++] = (unsigned char)((v >> 16) & 0xFF);
                if (s[i - 1] != '=') buf[n++] = (unsigned char)((v >> 8) & 0xFF);
                qn = 0;
            }
            continue;
        }
        int v = b64val(ch);
        if (v < 0) { aic_xfree(buf); return AIC_E_FORMAT; }
        quad[qn++] = v;
        if (qn == 4) {
            unsigned int x = ((unsigned int)quad[0] << 18) | ((unsigned int)quad[1] << 12) |
                             ((unsigned int)quad[2] << 6) | (unsigned int)quad[3];
            buf[n++] = (unsigned char)((x >> 16) & 0xFF);
            buf[n++] = (unsigned char)((x >> 8) & 0xFF);
            buf[n++] = (unsigned char)(x & 0xFF);
            qn = 0;
        }
    }
    if (qn == 2) {
        unsigned int x = ((unsigned int)quad[0] << 18) | ((unsigned int)quad[1] << 12);
        buf[n++] = (unsigned char)((x >> 16) & 0xFF);
    } else if (qn == 3) {
        unsigned int x = ((unsigned int)quad[0] << 18) | ((unsigned int)quad[1] << 12) |
                         ((unsigned int)quad[2] << 6);
        buf[n++] = (unsigned char)((x >> 16) & 0xFF);
        buf[n++] = (unsigned char)((x >> 8) & 0xFF);
    } else if (qn == 1) {
        aic_xfree(buf);
        return AIC_E_FORMAT;
    }
    *out = buf;
    *out_len = n;
    return AIC_OK;
}

