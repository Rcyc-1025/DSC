/* sync_clock.h —— 设备标识与逻辑时钟（技术设计 §16.5，FR-29）。
 *
 * 跨端消息顺序由逻辑时钟决定，不使用设备本地墙钟：
 *  - 本地新增消息时取本端已知最大 logical_ts 加 1；
 *  - 远端事件到达时沿用对端值，并把本地最大值抬到不低于该值；
 *  - 排序键为 (logical_ts, origin_device, seq)，两端得到一致序列；
 *  - 墙钟时间只用于界面展示，不参与排序。 */
#ifndef AIC_SYNC_CLOCK_H
#define AIC_SYNC_CLOCK_H

#include "aic_types.h"
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_SET_DEVICE_ID   "device.id"       /* 本机设备标识，UUID */
#define AIC_SET_DEVICE_NAME "device.name"     /* 本机设备名，展示用 */
#define AIC_SET_LOGICAL_TS  "sync.logical_ts" /* 逻辑时钟当前值 */

/* 读当前逻辑时钟值；键不存在时初始化为 0。 */
int aic_clock_peek(aic_db *db, long long *out_ts);
/* 本地新增实体：取本端最大值加 1 并返回新值（单语句原子自增）。 */
int aic_clock_next(aic_db *db, long long *out_ts);
/* 远端事件到达：本地最大值抬到不低于 remote_ts，返回抬高后的值。 */
int aic_clock_observe(aic_db *db, long long remote_ts);

/* 本机设备标识：不存在时生成 UUID 并落库。 */
int aic_device_id(aic_db *db, char *out, size_t cap);
/* 本机设备名：默认按平台取「Windows 端」「Android 端」，可由用户改名。 */
int aic_device_name(aic_db *db, char *out, size_t cap);
int aic_device_set_name(aic_db *db, const char *name);
const char *aic_device_default_name(void);

#ifdef __cplusplus
}
#endif

#endif
