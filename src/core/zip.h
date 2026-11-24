/* zip.h —— 最小 ZIP 读写（store 模式，无压缩，标准 ZIP 格式）。
 *
 * 用途：备份包（技术设计 5.5 / 8.3 / 14.2 要求「标准压缩格式并附带校验信息」）。
 * 只用 store（不压缩），因此不需要 zlib，也不会引入外部依赖；包内文件逐个做 SHA-256
 * 记在 manifest.json 里，解包时逐个校验。 */
#ifndef DSC_ZIP_H
#define DSC_ZIP_H

#include <stddef.h>

#include "dsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsc_zip_src {
    const char *name;   /* 包内相对路径，用 / 分隔 */
    const char *path;   /* 磁盘上的文件（与 data 二选一） */
    const char *data;   /* 内存内容（与 path 二选一） */
    size_t      len;    /* data 的长度 */
} dsc_zip_src;

/* 生成 zip；out_zip 已存在时覆盖。 */
int dsc_zip_create(const dsc_zip_src *srcs, size_t n, const char *out_zip,
                   char *err, size_t errlen);
/* 解包到 dest_dir（目录不存在会创建）；校验每个条目的 CRC32。 */
int dsc_zip_extract(const char *zip_path, const char *dest_dir, char *err, size_t errlen);
/* 读取包内某个条目到内存（堆分配，调用方 dsc_xfree）。 */
int dsc_zip_read_entry(const char *zip_path, const char *name, char **out, size_t *out_len,
                       char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DSC_ZIP_H */
