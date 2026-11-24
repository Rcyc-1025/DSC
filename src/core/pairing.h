/* pairing.h —— 双端配对（技术设计 §16.2，FR-26）。
 *
 * 规则（与文档逐条对应）：
 *  - 连接方与被连接方各自生成一个 6 位数字码（100000 至 999999），两端码值必须不同，重号时重新生成；
 *  - 只有「连接方输入的码 == 被连接方生成值」且「被连接方输入的码 == 连接方生成值」配对才成立；
 *  - 任一端输入错误即终止本次配对，不交换密钥；
 *  - 配对码只在内存中保存，有效期 5 分钟，累计 5 次输入错误作废并重新生成；
 *  - 配对成功后生成 device_pair_id 与 shared_secret，密钥写入平台安全存储，本地只保留引用。 */
#ifndef DSC_PAIRING_H
#define DSC_PAIRING_H

#include "dsc_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_PAIR_CODE_MIN     100000
#define DSC_PAIR_CODE_MAX     999999
#define DSC_PAIR_CODE_LEN     6
#define DSC_PAIR_CODE_TTL_MS  (5 * 60 * 1000) /* 有效期 5 分钟 */
#define DSC_PAIR_MAX_FAILS    5               /* 累计 5 次输入错误作废 */
#define DSC_CRED_SERVICE_SYNC "DSC-Sync"      /* 共享密钥在安全存储里的服务名 */
#define DSC_SET_PAIRING_PEER  "pairing.peer_id"

typedef enum dsc_pair_state {
    DSC_PAIR_IDLE = 0,
    DSC_PAIR_CODE_READY, /* 已生成本端码，等待与对端交换 */
    DSC_PAIR_VERIFIED,   /* 双向校验通过 */
    DSC_PAIR_FAILED      /* 错误次数超限或超时，需重新生成 */
} dsc_pair_state;

typedef struct dsc_pair_record {
    char      peer_id[40];
    char      peer_name[64];
    char      device_pair_id[40];
    char      secret_ref[128]; /* 形如 pair/<device_pair_id>，密钥本体在安全存储 */
    char      paired_at[32];
    long long last_logical_ts;
    int       realtime;
} dsc_pair_record;

typedef struct dsc_pairing dsc_pairing;

int  dsc_pairing_create(dsc_db *db, dsc_pairing **out);
void dsc_pairing_destroy(dsc_pairing *p);

dsc_pair_state dsc_pairing_state(const dsc_pairing *p);
const char    *dsc_pair_state_name(dsc_pair_state s);
const char    *dsc_pairing_code(const dsc_pairing *p);
long long      dsc_pairing_expires_at(const dsc_pairing *p);
int            dsc_pairing_fails(const dsc_pairing *p);

/* 生成本端配对码；avoid_code 是本端已知的对端码（可为 NULL），用于保证两端码值不同。 */
int dsc_pairing_generate(dsc_pairing *p, const char *avoid_code, long long now_ms,
                         char *out_code, size_t cap, char *err, size_t errlen);

/* 双向校验并建立配对。
 * peer_code 是我们输入的对端码（由对端确认过），peer_entered_our_code 是对端输入的本端码。
 * 任一项不合法即计一次错误；累计 5 次作废并清掉本端码。 */
int dsc_pairing_verify(dsc_pairing *p, const char *peer_id, const char *peer_name,
                       const char *peer_code, const char *peer_entered_our_code,
                       long long now_ms, dsc_pair_record *out, char *err, size_t errlen);

/* 配对码是否为合法的 6 位数字串。 */
int dsc_pair_code_valid(const char *code);

/* 查询当前已配对设备；未配对返回 DSC_E_NOTFOUND。 */
int dsc_pairing_current(dsc_db *db, dsc_pair_record *out);
/* 解除配对：删除配对关系与安全存储里的共享密钥，两端本地数据均保留。 */
int dsc_pairing_unpair(dsc_db *db, const char *peer_id, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
