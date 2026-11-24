/* notify.h —— 系统通知（技术设计 9.4：余额低于阈值时通知；14.1：更新可用时提示）。 */
#ifndef DSC_NOTIFY_H
#define DSC_NOTIFY_H

#include <stddef.h>

#include "dsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int dsc_notify_supported(void);
/* window 为平台窗口句柄（Windows 传 HWND）；Android 忽略。 */
int dsc_notify(void *window, const char *title, const char *body);

#ifdef __cplusplus
}
#endif

#endif /* DSC_NOTIFY_H */
