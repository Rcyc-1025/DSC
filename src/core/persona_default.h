/* persona_default.h —— 内置默认人设正文的外部声明。
 *
 * 正文由 tools/gen_persona.py 从 resources/persona/dsc_default_persona.txt 生成到 persona_default.c，
 * 这里只暴露一个常量；改文案改 txt 再重跑生成器，不要手改生成物。
 */
#ifndef DSC_PERSONA_DEFAULT_H
#define DSC_PERSONA_DEFAULT_H

/* 全新安装时写入 persona 表的默认人设正文（由 tools/gen_persona.py 生成，源文本在
 * resources/persona/dsc_default_persona.txt）。已有用户的人设不会被它覆盖。 */
extern const char DSC_PERSONA_DEFAULT_PROMPT[];

#endif /* DSC_PERSONA_DEFAULT_H */
