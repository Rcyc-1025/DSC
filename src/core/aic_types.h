/* aic_types.h —— 全项目共用的基础类型、错误码与常量。
 * 仅包含 C11 标准头，保证核心层可被 Windows / Android 交叉编译。 */
#ifndef AIC_TYPES_H
#define AIC_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一错误码。0 表示成功，负值表示失败，具体含义见 aic_strerror。 */
typedef enum aic_err {
    AIC_OK = 0,
    AIC_E_INVAL = -1,      /* 参数非法或字段校验失败 */
    AIC_E_NOTFOUND = -2,   /* 记录不存在 */
    AIC_E_CONFLICT = -3,   /* 冲突（唯一约束、状态不允许等） */
    AIC_E_LIMIT = -4,      /* 超出长度/数量上限 */
    AIC_E_DB = -5,         /* 数据库错误 */
    AIC_E_IO = -6,         /* 文件读写错误 */
    AIC_E_FORMAT = -7,     /* 数据格式错误（JSON、清单等） */
    AIC_E_CHECKSUM = -8,   /* 校验和不匹配 */
    AIC_E_VERSION = -9,    /* 版本号或 schema_version 不兼容 */
    AIC_E_SIGNATURE = -10, /* 签名校验失败 */
    AIC_E_TIMEOUT = -11,   /* 超时 */
    AIC_E_DISABLED = -12,  /* 插件/功能已禁用 */
    AIC_E_NOMEM = -13,     /* 内存不足 */
    AIC_E_BUSY = -14,      /* 资源忙（切换中、事务中） */
    AIC_E_STATE = -15,     /* 状态机不允许该操作 */
    AIC_E_AUTH = -16,      /* 凭证缺失或无效 */
    AIC_E_UNSUPPORTED = -17 /* 当前平台未实现 */
} aic_err;

const char *aic_strerror(int code);

/* 工作模式：问答模式（qa）与聊天模式（chat）。 */
typedef enum aic_mode {
    AIC_MODE_QA = 0,
    AIC_MODE_CHAT = 1
} aic_mode;

const char *aic_mode_name(aic_mode m);
aic_mode aic_mode_parse(const char *s);

/* 消息角色。 */
typedef enum aic_role {
    AIC_ROLE_USER = 0,
    AIC_ROLE_ASSISTANT = 1,
    AIC_ROLE_SYSTEM = 2
} aic_role;

const char *aic_role_name(aic_role r);

/* 主题模式：日光、夜间、跟随系统。 */
typedef enum aic_theme_pref {
    AIC_THEME_DAY = 0,
    AIC_THEME_NIGHT = 1,
    AIC_THEME_SYSTEM = 2
} aic_theme_pref;

/* 内置模型标识（与设计文档一致）。真实模型映射属于配置层，核心层不联网。 */
#define AIC_MODEL_FLASH "deepseekv4.1flash"
#define AIC_MODEL_PRO   "deepseekv4pro"

/* 导出格式常量。 */
#define AIC_EXPORT_FORMAT      "chat_record"
#define AIC_EXPORT_SCHEMA_VER  1
#define AIC_BACKUP_FORMAT      "aic_full_backup"
#define AIC_BACKUP_SCHEMA_VER  1

/* 字段长度上限（字符数，非字节）。 */
#define AIC_PROFILE_NAME_MAX      40
#define AIC_PROFILE_OCCUPATION_MAX 60
#define AIC_PROFILE_NOTE_MAX      500
#define AIC_AVATAR_MAX_EDGE       512
#define AIC_MESSAGE_MAX_BYTES     65536
#define AIC_CONTENT_MAX_BYTES     262144

/* 默认撤回时间窗（秒），可在设置中调整。 */
#define AIC_RECALL_WINDOW_DEFAULT 120
/* 默认上下文条数。 */
#define AIC_CONTEXT_MESSAGES_DEFAULT 20

#ifdef __cplusplus
}
#endif

#endif /* AIC_TYPES_H */
