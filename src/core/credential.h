/* credential.h —— 凭证安全存储（FR-02，安全章）。
 *
 * 目标：API Key 只进系统安全存储，界面与日志不出现明文；导出包永远不含凭证。
 * 平台实现：Windows 走凭据管理器（advapi32 的 CredWrite/CredRead/CredDelete），
 * Android 走系统安全存储（Keystore 包装的私有文件），由平台层提供同名实现。 */
#ifndef DSC_CREDENTIAL_H
#define DSC_CREDENTIAL_H

#include <stddef.h>

#include "dsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_CRED_SERVICE_DEFAULT "DSC"
#define DSC_CRED_ACCOUNT_APIKEY  "api_key"

/* 1 表示当前平台安全存储可用。 */
int dsc_cred_available(void);

/* 写入/覆盖；secret 必须是 UTF-8 明文（只在内存中出现）。 */
int dsc_cred_save(const char *service, const char *account, const char *secret,
                  char *err, size_t errlen);

/* 读取；不存在返回 DSC_E_NOTFOUND。out 收到后调用方负责 dsc_secure_zero。 */
int dsc_cred_load(const char *service, const char *account, char *out, size_t cap,
                  char *err, size_t errlen);

int dsc_cred_delete(const char *service, const char *account, char *err, size_t errlen);

/* 抹掉内存里的明文（volatile 写法防优化掉）。 */
void dsc_secure_zero(void *p, size_t n);

/* 日志/错误文本脱敏：把疑似凭证片段替换成 ***。 */
void dsc_cred_redact(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DSC_CREDENTIAL_H */
