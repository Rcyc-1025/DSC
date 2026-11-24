/* util.h —— 字符串缓冲、时间、UUID、SHA-256、UTF-8、路径与文件工具。 */
#ifndef DSC_UTIL_H
#define DSC_UTIL_H

#include "dsc_types.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 字符串缓冲 ---------- */
typedef struct sb {
    char  *data;
    size_t len;
    size_t cap;
} sb;

void  sb_init(sb *b);
void  sb_free(sb *b);
void  sb_reset(sb *b);
int   sb_reserve(sb *b, size_t need);
int   sb_appendn(sb *b, const char *s, size_t n);
int   sb_append(sb *b, const char *s);
int   sb_appendc(sb *b, char c);
int   sb_appendf(sb *b, const char *fmt, ...);
/* 追加后保证以 '\0' 结尾；data 永不为 NULL（空串时为 ""）。 */
const char *sb_cstr(sb *b);

char *dsc_strdup(const char *s);
char *dsc_strndup(const char *s, size_t n);
void *dsc_xmalloc(size_t n);
void *dsc_xcalloc(size_t n, size_t sz);
void *dsc_xrealloc(void *p, size_t n);
void  dsc_xfree(void *p);
/* 把 src 拷贝进固定缓冲区，超长截断并按 UTF-8 字符边界回退。 */
void  dsc_strlcpy_utf8(char *dst, size_t cap, const char *src);

/* ---------- UTF-8 ---------- */
size_t dsc_utf8_length(const char *s);           /* 码点个数 */
int    dsc_utf8_valid(const char *s);            /* 1 合法 */
size_t dsc_utf8_truncate_chars(char *s, size_t max_chars); /* 原地截断，返回新长度(字节) */

/* ---------- 时间 ---------- */
int64_t dsc_now_ms(void);
void    dsc_now_iso8601(char buf[32]);
void    dsc_ms_to_iso8601(int64_t ms, char buf[32]);
int64_t dsc_iso8601_to_ms(const char *s);
int     dsc_parse_iso8601(const char *s, int64_t *out_ms);

/* ---------- 随机源 ---------- */
/* 平台随机数（Windows RtlGenRandom / POSIX /dev/urandom），用于配对码与共享密钥。 */
int dsc_random_bytes(void *buf, size_t n);

/* ---------- UUID ---------- */
void dsc_uuid4(char out[37]);
int  dsc_uuid_valid(const char *s);

/* ---------- SHA-256 ---------- */
typedef struct dsc_sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buflen;
} dsc_sha256_ctx;

void dsc_sha256_init(dsc_sha256_ctx *c);
void dsc_sha256_update(dsc_sha256_ctx *c, const void *data, size_t len);
void dsc_sha256_final(dsc_sha256_ctx *c, uint8_t out[32]);
void dsc_sha256(const void *data, size_t len, uint8_t out[32]);
void dsc_hex_encode(const uint8_t *in, size_t n, char *out); /* out 需 2n+1 */
int  dsc_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len);
void dsc_sha256_hex(const void *data, size_t len, char out[65]);

/* ---------- 路径与文件 ---------- */
int  dsc_path_join(char *dst, size_t cap, const char *a, const char *b);
int  dsc_path_exists(const char *p);
int  dsc_is_dir(const char *p);
int  dsc_mkdir_p(const char *p);                 /* 递归创建目录 */
int  dsc_file_size(const char *p, int64_t *out);
int  dsc_file_read_all(const char *p, char **out, size_t *out_len);
int  dsc_file_write_all(const char *p, const void *data, size_t len);
int  dsc_file_append(const char *p, const void *data, size_t len);
int  dsc_file_copy(const char *src, const char *dst);
int  dsc_file_remove(const char *p);
int  dsc_file_move(const char *src, const char *dst);
const char *dsc_path_basename(const char *p);
/* 列出目录条目（跳过 . 与 ..），用 sep 拼接到 names_out。
 * 返回值：>=0 为条目数（空目录返回 0），负值为 DSC_E_*。调用方不要用 == DSC_OK 判断成功。 */
int  dsc_dir_list(const char *dir, sb *names_out, char sep);

/* ---------- 其他 ---------- */
int  dsc_str_ieq(const char *a, const char *b);
int  dsc_parse_int(const char *s, int *out);
int  dsc_parse_ll(const char *s, long long *out);
/* 简单正则无关的日期校验：YYYY-MM-DD 且月日范围合法。 */
int  dsc_is_valid_date(const char *s);
/* 相对路径校验：非空、非绝对路径、不含盘符、不含 ".."、不含反斜杠以外的协议前缀。 */
int  dsc_is_local_relative_path(const char *s);
/* 语义化版本比较：a<b 返回 -1，相等 0，a>b 返回 1。 */
int  dsc_version_compare(const char *a, const char *b);
/* 版本号是否合法（数字段用 . 分隔，至少两段，最多四段）。 */
int  dsc_version_valid(const char *v);
/* 是否恰为 need_len 位的十六进制串。 */
int  dsc_hex_valid(const char *hex, size_t need_len);
/* Base64 编解码（导出可选附带头像文件时用；标准表 + = 填充）。 */
int  dsc_base64_encode(const void *data, size_t len, sb *out);
int  dsc_base64_decode(const char *s, size_t len, unsigned char **out, size_t *out_len);

/* 敏感字符串扫描：命中返回 1，并把命中位置描述写入 hit。 */
int  dsc_scan_sensitive(const char *text, sb *hit);

#ifdef __cplusplus
}
#endif

#endif /* DSC_UTIL_H */
