/* credential.c —— 凭证安全存储的公共部分（脱敏、清零）。 */
#include "credential.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

void aic_secure_zero(void *p, size_t n)
{
    if (!p || !n) return;
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) *q++ = 0;
}

void aic_cred_redact(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;
    /* 先用敏感扫描定位，再把整段疑似凭证替换成 ***。 */
    sb hit;
    sb_init(&hit);
    int found = aic_scan_sensitive(in, &hit);
    sb_free(&hit);
    if (!found) {
        aic_strlcpy_utf8(out, cap, in);
        return;
    }
    /* 逐行处理：含敏感片段的行整行脱敏。 */
    size_t oi = 0;
    const char *line = in;
    while (*line && oi + 1 < cap) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[1024];
        size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, line, copy);
        buf[copy] = '\0';
        sb h;
        sb_init(&h);
        int bad = aic_scan_sensitive(buf, &h);
        sb_free(&h);
        const char *seg = bad ? "[已脱敏]" : buf;
        size_t sl = strlen(seg);
        if (oi + sl + 2 >= cap) break;
        memcpy(out + oi, seg, sl);
        oi += sl;
        if (nl) out[oi++] = '\n';
        else break;
        line = nl + 1;
    }
    out[oi] = '\0';
}
