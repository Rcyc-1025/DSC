/* prompt.h —— 提示词组装（FR-04 / 第 9.4 节）。
 * 组成顺序：基础指令 → 人设描述 → 用户资料变量 → 插件注入片段。
 * 变量在请求组装阶段替换，未填写替换为空串。 */
#ifndef DSC_PROMPT_H
#define DSC_PROMPT_H

#include "dsc_types.h"
#include "store_persona.h"
#include "store_profile.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_PROMPT_BASE \
    "你是一个本地运行的陪伴型对话助手，使用简体中文，除非用户明确要求其他语言。\n" \
    "回答要具体、不说空话；不知道就直说不知道。"

/* 把 {user_name} 等变量替换进 out（未填写的变量替换为空串）。 */
int dsc_prompt_substitute(const char *tpl, const dsc_profile *profile, sb *out);

/* 组装完整系统提示词；plugin_fragments 为已按扩展点顺序拼接好的插件片段（可为 NULL）。 */
int dsc_prompt_build(const dsc_persona *persona, const dsc_profile *profile,
                     const char *plugin_fragments, sb *out);

/* 生成调试视图（凭证脱敏后的提示词文本），供界面调试面板展示。 */
int dsc_prompt_debug_view(const dsc_persona *persona, const dsc_profile *profile,
                          const char *plugin_fragments, sb *out);

#ifdef __cplusplus
}
#endif

#endif
