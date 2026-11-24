/* theme.c —— 主题令牌与动效参数。 */
#include "theme.h"

#include <math.h>
#include <string.h>

/* FR-33：深蓝主色调（H≈213）。日光与夜间只改明度、不改色相，
 * 主色都由同一支深蓝色阶取出：日间 #103F7A、夜间 #6194C9。
 * 深蓝底 + 白字 10.4:1，夜间 3.2:1，均满足按钮 ≥3:1 的硬要求。 */
static const dsc_theme g_day = {
    0, "day",
    {
        0xF4F7FB, /* PAGE_BG */
        0xFFFFFF, /* CARD_BG */
        0x14203A, /* TEXT */
        0x5B6880, /* TEXT_SECONDARY */
        0xD9E2EF, /* DIVIDER */
        0x103F7A, /* PRIMARY：深蓝 */
        0x1F5FA8, /* ACCENT：同色相稍亮，用于强调与链接 */
        0x1C7A4A, /* SUCCESS */
        0x9A6100, /* WARNING */
        0xB3261E  /* DANGER */
    }
};

static const dsc_theme g_night = {
    1, "night",
    {
        0x0B1220, /* PAGE_BG */
        0x141D2E, /* CARD_BG */
        0xE6EDF8, /* TEXT */
        0x9EACC4, /* TEXT_SECONDARY */
        0x26334A, /* DIVIDER */
        0x6194C9, /* PRIMARY：同色相提亮 */
        0x4E86D6, /* ACCENT */
        0x4CC38A, /* SUCCESS */
        0xE0A33A, /* WARNING */
        0xF07068  /* DANGER */
    }
};

const dsc_theme *dsc_theme_day(void) { return &g_day; }
const dsc_theme *dsc_theme_night(void) { return &g_night; }

const dsc_theme *dsc_theme_resolve(int pref, int system_night)
{
    if (pref == 0) return &g_day;
    if (pref == 1) return &g_night;
    return system_night ? &g_night : &g_day;
}

const char *dsc_token_name(dsc_token t)
{
    switch (t) {
    case DSC_TOK_PAGE_BG:        return "page_bg";
    case DSC_TOK_CARD_BG:        return "card_bg";
    case DSC_TOK_TEXT:           return "text";
    case DSC_TOK_TEXT_SECONDARY: return "text_secondary";
    case DSC_TOK_DIVIDER:        return "divider";
    case DSC_TOK_PRIMARY:        return "primary";
    case DSC_TOK_ACCENT:         return "accent";
    case DSC_TOK_SUCCESS:        return "success";
    case DSC_TOK_WARNING:        return "warning";
    case DSC_TOK_DANGER:         return "danger";
    default:                     return "unknown";
    }
}

unsigned int dsc_theme_color(const dsc_theme *th, dsc_token t)
{
    if (!th || (int)t < 0 || (int)t >= DSC_TOK_COUNT) return 0;
    return th->color[t];
}

/* WCAG 2.x 相对亮度：先做 sRGB 反伽马，再按 0.2126/0.7152/0.0722 加权。 */
static double srgb_to_linear(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double rel_lum(unsigned int rgb)
{
    double r = srgb_to_linear((double)((rgb >> 16) & 0xFF) / 255.0);
    double g = srgb_to_linear((double)((rgb >> 8) & 0xFF) / 255.0);
    double b = srgb_to_linear((double)(rgb & 0xFF) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

int dsc_theme_contrast(unsigned int fg, unsigned int bg)
{
    double l1 = rel_lum(fg);
    double l2 = rel_lum(bg);
    if (l1 < l2) { double t = l1; l1 = l2; l2 = t; }
    double ratio = (l1 + 0.05) / (l2 + 0.05);
    return (int)(ratio * 100.0 + 0.5);
}

int dsc_theme_contrast_ok(unsigned int fg, unsigned int bg, int min_x100)
{
    return dsc_theme_contrast(fg, bg) >= min_x100 ? 1 : 0;
}

int dsc_theme_audit(const dsc_theme *th)
{
    if (!th) return 1;
    int bad = 0;
    /* 正文与次要文字要过 4.5:1（次要文字用在卡片上）。 */
    if (!dsc_theme_contrast_ok(th->color[DSC_TOK_TEXT], th->color[DSC_TOK_PAGE_BG], 450)) bad++;
    if (!dsc_theme_contrast_ok(th->color[DSC_TOK_TEXT], th->color[DSC_TOK_CARD_BG], 450)) bad++;
    if (!dsc_theme_contrast_ok(th->color[DSC_TOK_TEXT_SECONDARY], th->color[DSC_TOK_CARD_BG], 450)) bad++;
    /* 关键按钮（主色底 + 白字）至少 3:1。 */
    if (!dsc_theme_contrast_ok(0xFFFFFF, th->color[DSC_TOK_PRIMARY], 300)) bad++;
    return bad;
}

dsc_motion dsc_motion_default(void)
{
    dsc_motion m;
    m.mode_switch_ms = 220;
    m.tab_switch_ms = 180;
    m.subpage_ms = 200;
    m.overlay_ms = 160;
    m.message_ms = 140;
    m.toast_ms = 120;
    m.reduced = 0;
    m.easing_enter = "ease-out";
    m.easing_switch = "ease-in-out";
    return m;
}

dsc_motion dsc_motion_reduced(void)
{
    dsc_motion m = dsc_motion_default();
    m.mode_switch_ms = 0;
    m.tab_switch_ms = 0;
    m.subpage_ms = 0;
    m.overlay_ms = 0;
    m.message_ms = 0;
    m.toast_ms = 0;
    m.reduced = 1;
    return m;
}
