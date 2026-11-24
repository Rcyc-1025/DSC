/* dsc_types.h —— 全项目共用的基础类型、错误码与常量。
 * 仅包含 C11 标准头，保证核心层可被 Windows / Android 交叉编译。 */
#ifndef DSC_TYPES_H
#define DSC_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一错误码。0 表示成功，负值表示失败，具体含义见 dsc_strerror。 */
typedef enum dsc_err {
    DSC_OK = 0,
    DSC_E_INVAL = -1,      /* 参数非法或字段校验失败 */
    DSC_E_NOTFOUND = -2,   /* 记录不存在 */
    DSC_E_CONFLICT = -3,   /* 冲突（唯一约束、状态不允许等） */
    DSC_E_LIMIT = -4,      /* 超出长度/数量上限 */
    DSC_E_DB = -5,         /* 数据库错误 */
    DSC_E_IO = -6,         /* 文件读写错误 */
    DSC_E_FORMAT = -7,     /* 数据格式错误（JSON、清单等） */
    DSC_E_CHECKSUM = -8,   /* 校验和不匹配 */
    DSC_E_VERSION = -9,    /* 版本号或 schema_version 不兼容 */
    DSC_E_SIGNATURE = -10, /* 签名校验失败 */
    DSC_E_TIMEOUT = -11,   /* 超时 */
    DSC_E_DISABLED = -12,  /* 插件/功能已禁用 */
    DSC_E_NOMEM = -13,     /* 内存不足 */
    DSC_E_BUSY = -14,      /* 资源忙（切换中、事务中） */
    DSC_E_STATE = -15,     /* 状态机不允许该操作 */
    DSC_E_AUTH = -16,      /* 凭证缺失或无效 */
    DSC_E_UNSUPPORTED = -17 /* 当前平台未实现 */
} dsc_err;

const char *dsc_strerror(int code);

/* 工作模式：问答模式（qa）与聊天模式（chat）。 */
typedef enum dsc_mode {
    DSC_MODE_QA = 0,
    DSC_MODE_CHAT = 1
} dsc_mode;

const char *dsc_mode_name(dsc_mode m);
dsc_mode dsc_mode_parse(const char *s);

/* 消息角色。 */
typedef enum dsc_role {
    DSC_ROLE_USER = 0,
    DSC_ROLE_ASSISTANT = 1,
    DSC_ROLE_SYSTEM = 2
} dsc_role;

const char *dsc_role_name(dsc_role r);

/* 主题模式：日光、夜间、跟随系统。 */
typedef enum dsc_theme_pref {
    DSC_THEME_DAY = 0,
    DSC_THEME_NIGHT = 1,
    DSC_THEME_SYSTEM = 2
} dsc_theme_pref;

/* 内置模型标识（与设计文档一致）。真实模型映射属于配置层，核心层不联网。 */
#define DSC_MODEL_FLASH "deepseekv4.1flash"
#define DSC_MODEL_PRO   "deepseekv4pro"

/* 导出格式常量。 */
#define DSC_EXPORT_FORMAT      "chat_record"
#define DSC_EXPORT_SCHEMA_VER  1
#define DSC_BACKUP_FORMAT      "dsc_full_backup"
#define DSC_BACKUP_SCHEMA_VER  1

/* 字段长度上限（字符数，非字节）。 */
#define DSC_PROFILE_NAME_MAX      40
#define DSC_PROFILE_OCCUPATION_MAX 60
#define DSC_PROFILE_NOTE_MAX      500
#define DSC_AVATAR_MAX_EDGE       512
#define DSC_MESSAGE_MAX_BYTES     65536
#define DSC_CONTENT_MAX_BYTES     262144

/* 默认撤回时间窗（秒），可在设置中调整。 */
#define DSC_RECALL_WINDOW_DEFAULT 120
/* 默认上下文条数。 */
#define DSC_CONTEXT_MESSAGES_DEFAULT 20

#ifdef __cplusplus
}
#endif

#endif /* DSC_TYPES_H */
