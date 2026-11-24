/* zip.c */
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static unsigned long g_crc_table[256];
static int g_crc_ready;

static void crc_init(void)
{
    if (g_crc_ready) return;
    for (unsigned long i = 0; i < 256; i++) {
        unsigned long c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = 1;
}

static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    crc_init();
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); }
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned get16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

int dsc_zip_create(const dsc_zip_src *srcs, size_t n, const char *out_zip, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!out_zip || (n && !srcs)) return DSC_E_INVAL;
    crc_init();
    FILE *fp = fopen(out_zip, "wb");
    if (!fp) {
        if (err && errlen) snprintf(err, errlen, "无法创建压缩包：%s", out_zip);
        return DSC_E_IO;
    }
    unsigned long *offsets = (unsigned long *)dsc_xcalloc(n ? n : 1, sizeof(unsigned long));
    unsigned long *crcs = (unsigned long *)dsc_xcalloc(n ? n : 1, sizeof(unsigned long));
    unsigned long *sizes = (unsigned long *)dsc_xcalloc(n ? n : 1, sizeof(unsigned long));
    int rc = DSC_OK;

    for (size_t i = 0; i < n; i++) {
        char *data = NULL;
        size_t len = 0;
        if (srcs[i].data) {
            data = (char *)srcs[i].data;
            len = srcs[i].len;
        } else {
            int r = dsc_file_read_all(srcs[i].path, &data, &len);
            if (r != DSC_OK) {
                if (err && errlen) snprintf(err, errlen, "读取待备份文件失败：%s", srcs[i].path);
                rc = r;
                break;
            }
        }
        unsigned long crc = crc32_of((const unsigned char *)data, len);
        size_t nlen = strlen(srcs[i].name);
        offsets[i] = (unsigned long)ftell(fp);
        crcs[i] = crc;
        sizes[i] = (unsigned long)len;

        unsigned char h[30];
        put32(h, 0x04034b50UL);
        put16(h + 4, 20);
        put16(h + 6, 0x0800);          /* UTF-8 名称 */
        put16(h + 8, 0);               /* store */
        put16(h + 10, 0); put16(h + 12, 0x21);  /* 时间/日期：固定值，保证可复现 */
        put32(h + 14, crc);
        put32(h + 18, (unsigned long)len);
        put32(h + 22, (unsigned long)len);
        put16(h + 26, (unsigned)nlen);
        put16(h + 28, 0);
        if (fwrite(h, 1, 30, fp) != 30 || fwrite(srcs[i].name, 1, nlen, fp) != nlen ||
            (len && fwrite(data, 1, len, fp) != len)) {
            if (err && errlen) snprintf(err, errlen, "写入压缩包失败：%s", out_zip);
            rc = DSC_E_IO;
            break;
        }
        if (!srcs[i].data) dsc_xfree(data);
    }

    unsigned long cd_start = (unsigned long)ftell(fp);
    if (rc == DSC_OK) {
        for (size_t i = 0; i < n; i++) {
            size_t nlen = strlen(srcs[i].name);
            unsigned char c[46];
            put32(c, 0x02014b50UL);
            put16(c + 4, 20);
            put16(c + 6, 20);
            put16(c + 8, 0x0800);
            put16(c + 10, 0);
            put16(c + 12, 0); put16(c + 14, 0x21);
            put32(c + 16, crcs[i]);
            put32(c + 20, sizes[i]);
            put32(c + 24, sizes[i]);
            put16(c + 28, (unsigned)nlen);
            put16(c + 30, 0);
            put16(c + 32, 0);
            put16(c + 34, 0);
            put16(c + 36, 0);
            put32(c + 38, 0);
            put32(c + 42, offsets[i]);
            if (fwrite(c, 1, 46, fp) != 46 || fwrite(srcs[i].name, 1, nlen, fp) != nlen) { rc = DSC_E_IO; break; }
        }
    }
    unsigned long cd_size = (unsigned long)ftell(fp) - cd_start;
    if (rc == DSC_OK) {
        unsigned char e[22];
        put32(e, 0x06054b50UL);
        put16(e + 4, 0); put16(e + 6, 0);
        put16(e + 8, (unsigned)(n > 0xFFFF ? 0xFFFF : n));
        put16(e + 10, (unsigned)(n > 0xFFFF ? 0xFFFF : n));
        put32(e + 12, cd_size);
        put32(e + 16, cd_start);
        put16(e + 20, 0);
        if (fwrite(e, 1, 22, fp) != 22) rc = DSC_E_IO;
    }
    fclose(fp);
    dsc_xfree(offsets); dsc_xfree(crcs); dsc_xfree(sizes);
    if (rc != DSC_OK && err && errlen && !err[0]) dsc_strlcpy_utf8(err, errlen, "生成压缩包失败");
    if (rc != DSC_OK) dsc_file_remove(out_zip);
    return rc;
}

/* 读整个 zip 到内存并定位中央目录。 */
typedef struct zip_view {
    unsigned char *buf;
    size_t         len;
    size_t         cd_start;
    size_t         cd_count;
} zip_view;

static int zip_open(const char *path, zip_view *v, char *err, size_t errlen)
{
    memset(v, 0, sizeof(*v));
    char *data = NULL;
    size_t len = 0;
    int rc = dsc_file_read_all(path, &data, &len);
    if (rc != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "无法读取压缩包：%s", path);
        return rc;
    }
    v->buf = (unsigned char *)data;
    v->len = len;
    size_t scan_from = len > 65557 ? len - 65557 : 0;
    long eocd = -1;
    for (size_t i = len >= 22 ? len - 22 : 0; i + 22 <= len; i--) {
        if (get32(v->buf + i) == 0x06054b50UL) { eocd = (long)i; break; }
        if (i == scan_from) break;
    }
    if (eocd < 0) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "压缩包格式无效（找不到中央目录）");
        dsc_xfree(v->buf); v->buf = NULL;
        return DSC_E_FORMAT;
    }
    v->cd_count = get16(v->buf + eocd + 10);
    v->cd_start = get32(v->buf + eocd + 16);
    if (v->cd_start >= len) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "压缩包中央目录越界");
        dsc_xfree(v->buf); v->buf = NULL;
        return DSC_E_FORMAT;
    }
    return DSC_OK;
}

static void zip_close(zip_view *v)
{
    dsc_xfree(v->buf);
    v->buf = NULL;
}

/* 遍历中央目录；cb 收到名称、数据指针、长度、CRC。 */
typedef int (*zip_entry_fn)(void *ud, const char *name, const unsigned char *data,
                            size_t len, unsigned long crc, char *err, size_t errlen);

static int zip_foreach(zip_view *v, zip_entry_fn cb, void *ud, char *err, size_t errlen)
{
    size_t pos = v->cd_start;
    for (size_t i = 0; i < v->cd_count; i++) {
        if (pos + 46 > v->len) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "压缩包目录项损坏");
            return DSC_E_FORMAT;
        }
        if (get32(v->buf + pos) != 0x02014b50UL) {
            if (err && errlen) dsc_strlcpy_utf8(err, errlen, "压缩包目录项签名不符");
            return DSC_E_FORMAT;
        }
        unsigned long crc = get32(v->buf + pos + 16);
        unsigned long csize = get32(v->buf + pos + 20);
        unsigned long usize = get32(v->buf + pos + 24);
        unsigned nlen = get16(v->buf + pos + 28);
        unsigned elen = get16(v->buf + pos + 30);
        unsigned clen = get16(v->buf + pos + 32);
        unsigned long lho = get32(v->buf + pos + 42);
        if (pos + 46 + nlen > v->len) return DSC_E_FORMAT;
        char name[512];
        if (nlen >= sizeof(name)) return DSC_E_FORMAT;
        memcpy(name, v->buf + pos + 46, nlen);
        name[nlen] = '\0';
        if (lho + 30 > v->len) return DSC_E_FORMAT;
        unsigned lnlen = get16(v->buf + lho + 26);
        unsigned lelen = get16(v->buf + lho + 28);
        size_t data_off = (size_t)lho + 30 + lnlen + lelen;
        if (data_off + csize > v->len) return DSC_E_FORMAT;
        if (get16(v->buf + pos + 10) != 0) {
            if (err && errlen) snprintf(err, errlen, "压缩包使用了不支持的压缩方式：%s", name);
            return DSC_E_UNSUPPORTED;
        }
        int rc = cb(ud, name, v->buf + data_off, usize, crc, err, errlen);
        if (rc != DSC_OK) return rc;
        pos += 46 + nlen + elen + clen;
    }
    return DSC_OK;
}

typedef struct extract_ctx {
    const char *dest;
} extract_ctx;

static int extract_one(void *ud, const char *name, const unsigned char *data, size_t len,
                       unsigned long crc, char *err, size_t errlen)
{
    extract_ctx *c = (extract_ctx *)ud;
    if (strstr(name, "..")) {
        if (err && errlen) snprintf(err, errlen, "压缩包内路径非法：%s", name);
        return DSC_E_FORMAT;
    }
    if (crc32_of(data, len) != crc) {
        if (err && errlen) snprintf(err, errlen, "压缩包内文件校验失败：%s", name);
        return DSC_E_CHECKSUM;
    }
    char path[1024];
    if (dsc_path_join(path, sizeof(path), c->dest, name) != DSC_OK) return DSC_E_INVAL;
    /* 名称里可能带子目录：逐级创建。 */
    char dir[1024];
    dsc_strlcpy_utf8(dir, sizeof(dir), path);
    for (char *p = dir + strlen(c->dest) + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            dsc_mkdir_p(dir);
            *p = save;
        }
    }
    int r = dsc_file_write_all(path, (const char *)data, len);
    if (r != DSC_OK) {
        if (err && errlen) snprintf(err, errlen, "写入解包文件失败：%s", path);
        return r;
    }
    return DSC_OK;
}

int dsc_zip_extract(const char *zip_path, const char *dest_dir, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!zip_path || !dest_dir) return DSC_E_INVAL;
    int rc = dsc_mkdir_p(dest_dir);
    if (rc != DSC_OK) return rc;
    zip_view v;
    rc = zip_open(zip_path, &v, err, errlen);
    if (rc != DSC_OK) return rc;
    extract_ctx ctx;
    ctx.dest = dest_dir;
    rc = zip_foreach(&v, extract_one, &ctx, err, errlen);
    zip_close(&v);
    return rc;
}

typedef struct read_ctx {
    const char *want;
    char       *out;
    size_t      len;
    size_t      cap;
    int         found;
} read_ctx;

static int read_one(void *ud, const char *name, const unsigned char *data, size_t len,
                    unsigned long crc, char *err, size_t errlen)
{
    read_ctx *c = (read_ctx *)ud;
    if (!dsc_str_ieq(name, c->want)) return DSC_OK;
    (void)crc; (void)err; (void)errlen;
    c->out = (char *)dsc_xmalloc(len + 1);
    memcpy(c->out, data, len);
    c->out[len] = '\0';
    c->len = len;
    c->found = 1;
    return DSC_E_NOTFOUND;   /* 找到即停止遍历（复用返回码，不当错误处理） */
}

int dsc_zip_read_entry(const char *zip_path, const char *name, char **out, size_t *out_len,
                       char *err, size_t errlen)
{
    if (!zip_path || !name || !out) return DSC_E_INVAL;
    *out = NULL;
    if (out_len) *out_len = 0;
    zip_view v;
    int rc = zip_open(zip_path, &v, err, errlen);
    if (rc != DSC_OK) return rc;
    read_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.want = name;
    rc = zip_foreach(&v, read_one, &ctx, err, errlen);
    zip_close(&v);
    if (!ctx.found) {
        if (err && errlen && !err[0]) snprintf(err, errlen, "压缩包内没有该文件：%s", name);
        return DSC_E_NOTFOUND;
    }
    *out = ctx.out;
    if (out_len) *out_len = ctx.len;
    return DSC_OK;
}
