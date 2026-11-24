/* theme.h —— 主题令牌与动效参数（FR-17 页面配色、FR-21 页面切换动画）。
 *
 * 规则：界面层不许出现硬编码色值，一律按令牌取值；切换日间/夜间只替换令牌映射，
 * 不重建页面树。正文对比度 >= 4.5:1，关键按钮 >= 3:1（WCAG AA）。 */
#ifndef DSC_THEME_H
#define DSC_THEME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dsc_token {
    DSC_TOK_PAGE_BG = 0,   /* 页面背景 */
    DSC_TOK_CARD_BG,       /* 卡片背景 */
    DSC_TOK_TEXT,          /* 正文 */
    DSC_TOK_TEXT_SECONDARY,/* 次要文字 */
    DSC_TOK_DIVIDER,       /* 分割线 */
    DSC_TOK_PRIMARY,       /* 主色（发送按钮等） */
    DSC_TOK_ACCENT,        /* 强调色（角标、选中态） */
    DSC_TOK_SUCCESS,
    DSC_TOK_WARNING,
    DSC_TOK_DANGER,
    DSC_TOK_COUNT
} dsc_token;

typedef struct dsc_theme {
    int          night;      /* 0 日间 / 1 夜间 */
    const char  *name;       /* 令牌表名（调试用） */
    unsigned int color[DSC_TOK_COUNT]; /* 0xRRGGBB */
} dsc_theme;

/* pref: 0 日光 / 1 夜间 / 2 跟随系统；system_night 是系统当前是否夜间。 */
const dsc_theme *dsc_theme_resolve(int pref, int system_night);
const dsc_theme *dsc_theme_day(void);
const dsc_theme *dsc_theme_night(void);
const char *dsc_token_name(dsc_token t);
/* 返回 0xRRGGBB；越界返回 0。 */
unsigned int dsc_theme_color(const dsc_theme *th, dsc_token t);

/* 对比度（WCAG 公式），返回值放大 100 倍：450 表示 4.5:1。 */
int dsc_theme_contrast(unsigned int fg, unsigned int bg);
/* 1 表示达标。 */
int dsc_theme_contrast_ok(unsigned int fg, unsigned int bg, int min_x100);
/* 检查一套令牌的硬性对比度要求，返回不达标项数量。 */
int dsc_theme_audit(const dsc_theme *th);

/* ---------- 动效（FR-21） ---------- */
typedef struct dsc_motion {
    int mode_switch_ms;  /* 220 */
    int tab_switch_ms;   /* 180 */
    int subpage_ms;      /* 200 */
    int overlay_ms;      /* 160 */
    int message_ms;      /* 140 */
    int toast_ms;        /* 120 */
    int reduced;         /* 系统"减少动态效果"：所有时长归零 */
    const char *easing_enter;
    const char *easing_switch;
} dsc_motion;

dsc_motion dsc_motion_default(void);
/* 系统开启"减少动态效果"时返回全部为 0 的动效参数。 */
dsc_motion dsc_motion_reduced(void);
/* 单帧预算（毫秒）：33.3ms*0.5 -> 16.7ms */
#define DSC_FRAME_BUDGET_MS 16.7

#ifdef __cplusplus
}
#endif

#endif /* DSC_THEME_H */
