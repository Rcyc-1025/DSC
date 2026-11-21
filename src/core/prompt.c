/* prompt.c */
#include "prompt.h"

#include <string.h>

typedef struct var_map {
    const char *name;
    const char *value;
} var_map;

/* 变量替换：一遍扫描，避免替换结果再次被解释。 */
static void substitute_into(sb *out, const char *tpl, const var_map *vars, size_t nvars)
{
    if (!tpl) return;
    const char *p = tpl;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end) {
                size_t nlen = (size_t)(end - (p + 1));
                int matched = 0;
                for (size_t i = 0; i < nvars; i++) {
                    if (strlen(vars[i].name) == nlen && strncmp(vars[i].name, p + 1, nlen) == 0) {
                        if (vars[i].value && *vars[i].value) sb_append(out, vars[i].value);
                        matched = 1;
                        break;
                    }
                }
                if (matched) {
                    p = end + 1;
                    continue;
                }
            }
        }
        sb_appendc(out, *p);
        p++;
    }
}

int aic_prompt_substitute(const char *tpl, const aic_profile *profile, sb *out)
{
    if (!out) return AIC_E_INVAL;
    var_map vars[5];
    aic_profile empty;
    if (!profile) {
        memset(&empty, 0, sizeof(empty));
        profile = &empty;
    }
    vars[0].name = "user_name";       vars[0].value = profile->name;
    vars[1].name = "user_gender";     vars[1].value = profile->gender;
    vars[2].name = "user_birthday";   vars[2].value = profile->birthday;
    vars[3].name = "user_occupation"; vars[3].value = profile->occupation;
    vars[4].name = "user_extra_note"; vars[4].value = profile->extra_note;
    substitute_into(out, tpl, vars, 5);
    return AIC_OK;
}

static void append_block(sb *out, const char *title, const char *body)
{
    if (!body || !*body) return;
    if (out->len) sb_append(out, "\n\n");
    if (title && *title) {
        sb_append(out, title);
        sb_append(out, "\n");
    }
    sb_append(out, body);
}

int aic_prompt_build(const aic_persona *persona, const aic_profile *profile,
                     const char *plugin_fragments, sb *out)
{
    if (!out) return AIC_E_INVAL;
    sb_init(out);

    sb base;
    sb_init(&base);
    aic_prompt_substitute(AIC_PROMPT_BASE, profile, &base);
    sb_append(out, sb_cstr(&base));
    sb_free(&base);

    if (persona) {
        sb desc;
        sb_init(&desc);
        aic_prompt_substitute(persona->description, profile, &desc);
        append_block(out, "【人设】", sb_cstr(&desc));
        sb_free(&desc);

        sb prompt;
        sb_init(&prompt);
        aic_prompt_substitute(persona->base_prompt ? persona->base_prompt : "", profile, &prompt);
        append_block(out, NULL, sb_cstr(&prompt));
        sb_free(&prompt);
    }

    if (profile) {
        sb facts;
        sb_init(&facts);
        if (profile->name[0]) sb_appendf(&facts, "- 姓名：%s\n", profile->name);
        if (profile->gender[0] && strcmp(profile->gender, "未设置") != 0)
            sb_appendf(&facts, "- 性别：%s\n", profile->gender);
        if (profile->birthday[0]) sb_appendf(&facts, "- 生日：%s\n", profile->birthday);
        if (profile->occupation[0]) sb_appendf(&facts, "- 职业：%s\n", profile->occupation);
        if (profile->extra_note[0]) sb_appendf(&facts, "- 补充说明：%s\n", profile->extra_note);
        if (facts.len) append_block(out, "【用户资料】", sb_cstr(&facts));
        sb_free(&facts);
    }

    if (plugin_fragments && *plugin_fragments) append_block(out, "【插件扩展】", plugin_fragments);
    return AIC_OK;
}

int aic_prompt_debug_view(const aic_persona *persona, const aic_profile *profile,
                          const char *plugin_fragments, sb *out)
{
    if (!out) return AIC_E_INVAL;
    sb tmp;
    int rc = aic_prompt_build(persona, profile, plugin_fragments, &tmp);
    if (rc != AIC_OK) return rc;
    sb_init(out);
    /* 调试面板：整体展示，但必须过一遍敏感串扫描，命中即脱敏。 */
    sb hit;
    sb_init(&hit);
    if (aic_scan_sensitive(sb_cstr(&tmp), &hit)) {
        sb_append(out, "（提示词包含疑似凭证内容，已隐藏；命中位置：");
        sb_append(out, sb_cstr(&hit));
        sb_append(out, "）");
    } else {
        sb_append(out, sb_cstr(&tmp));
    }
    sb_free(&hit);
    sb_free(&tmp);
    return AIC_OK;
}
