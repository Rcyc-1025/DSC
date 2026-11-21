/* diaglog.h —— 诊断日志（技术设计 15.2：解析失败保留原始响应到诊断日志）。
 *
 * 约束：日志里绝不出现凭证明文 —— 逐行过敏感串扫描，命中的行整行丢弃并留一行占位，
 * 因此即使模型服务把 Key 回显在错误体里也不会落盘。 */
#ifndef AIC_DIAGLOG_H
#define AIC_DIAGLOG_H

#include <stddef.h>

#include "aic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_LOG_MAX_LINE 4096
#define AIC_LOG_ROTATE_BYTES (4 * 1024 * 1024)

/* 在 dir 下按天开日志（app-YYYYMMDD.log），可重复调用（换目录会先关旧文件）。 */
int  aic_log_init(const char *dir);
void aic_log_close(void);
/* 当前日志文件绝对路径，未初始化返回空串。 */
const char *aic_log_path(void);
/* level 取 INFO / WARN / ERROR。 */
void aic_log_write(const char *level, const char *fmt, ...);
/* 原始响应：脱敏后按行写入，超过 8KB 截断并注明截断长度。 */
void aic_log_blob(const char *tag, const char *data, size_t len);
/* 把最后一条错误（含分类与文案）记一行。 */
void aic_log_error(const char *where, int code, const char *err);

#ifdef __cplusplus
}
#endif

#endif /* AIC_DIAGLOG_H */
