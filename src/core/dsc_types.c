/* dsc_types.c —— 错误码与枚举的字符串映射。 */
#include "dsc_types.h"

#include <string.h>

const char *dsc_strerror(int code)
{
    switch (code) {
    case DSC_OK:            return "成功";
    case DSC_E_INVAL:       return "参数非法或校验失败";
    case DSC_E_NOTFOUND:    return "记录不存在";
    case DSC_E_CONFLICT:    return "冲突";
    case DSC_E_LIMIT:       return "超出上限";
    case DSC_E_DB:          return "数据库错误";
    case DSC_E_IO:          return "文件读写错误";
    case DSC_E_FORMAT:      return "数据格式错误";
    case DSC_E_CHECKSUM:    return "校验和不匹配";
    case DSC_E_VERSION:     return "版本不兼容";
    case DSC_E_SIGNATURE:   return "签名校验失败";
    case DSC_E_TIMEOUT:     return "操作超时";
    case DSC_E_DISABLED:    return "功能或插件已禁用";
    case DSC_E_NOMEM:       return "内存不足";
    case DSC_E_BUSY:        return "资源忙";
    case DSC_E_STATE:       return "当前状态不允许该操作";
    case DSC_E_AUTH:        return "凭证缺失或无效";
    case DSC_E_UNSUPPORTED: return "当前平台不支持";
    default:                return "未知错误";
    }
}

const char *dsc_mode_name(dsc_mode m)
{
    switch (m) {
    case DSC_MODE_QA:   return "qa";
    case DSC_MODE_CHAT: return "chat";
    default:            return NULL;
    }
}

dsc_mode dsc_mode_parse(const char *s)
{
    if (!s) return DSC_MODE_CHAT;
    if (strcmp(s, "qa") == 0) return DSC_MODE_QA;
    if (strcmp(s, "chat") == 0) return DSC_MODE_CHAT;
    return DSC_MODE_CHAT;
}

const char *dsc_role_name(dsc_role r)
{
    switch (r) {
    case DSC_ROLE_USER:      return "user";
    case DSC_ROLE_ASSISTANT: return "assistant";
    case DSC_ROLE_SYSTEM:    return "system";
    default:                 return NULL;
    }
}
