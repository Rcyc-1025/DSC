/* brand_win.c —— 把内嵌位图（RCDATA）画到窗口上，用 GDI+ 解码 + 缩放。
 *
 * 不走 avatar_win.c：那里只从磁盘文件加载，而品牌插图是编进 exe 的资源。
 * 用 SHCreateMemStream 把资源字节包成 IStream 交给 GdipLoadImageFromStream，
 * 因此不需要链接 ole32/shlwapi（两者都动态取）。
 */
#include "brand.h"

#include <windows.h>
#include <objidl.h>

#include <string.h>

typedef struct gp_guid {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} gp_guid;

typedef struct gp_startup_input {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} gp_startup_input;

typedef int(WINAPI *fn_startup)(ULONG_PTR *, const gp_startup_input *, void *);
typedef int(WINAPI *fn_load_stream)(IStream *, void **);
typedef int(WINAPI *fn_get_w)(void *, UINT *);
typedef int(WINAPI *fn_get_h)(void *, UINT *);
typedef int(WINAPI *fn_dispose)(void *);
typedef int(WINAPI *fn_from_hdc)(HDC, void **);
typedef int(WINAPI *fn_draw_rect)(void *, void *, int, int, int, int);
typedef int(WINAPI *fn_del_graphics)(void *);
typedef IStream *(WINAPI *fn_mem_stream)(const BYTE *, UINT);

static struct {
    int             ready;
    int             failed;
    ULONG_PTR       token;
    fn_startup      startup;
    fn_load_stream  load_stream;
    fn_get_w        get_w;
    fn_get_h        get_h;
    fn_dispose      dispose;
    fn_from_hdc     from_hdc;
    fn_draw_rect    draw_rect;
    fn_del_graphics del_graphics;
    fn_mem_stream   mem_stream;
} bg;

static int bg_init(void)
{
    if (bg.ready) return 1;
    if (bg.failed) return 0;
    {
        HMODULE gp_lib = LoadLibraryW(L"gdiplus.dll");
        HMODULE sh_lib = LoadLibraryW(L"shlwapi.dll");
        if (!gp_lib || !sh_lib) { bg.failed = 1; return 0; }
#define BG_GET(field, lib, name) \
        *(FARPROC *)&bg.field = GetProcAddress(lib, name); \
        if (!bg.field) { bg.failed = 1; return 0; }
        BG_GET(startup, gp_lib, "GdiplusStartup");
        BG_GET(load_stream, gp_lib, "GdipLoadImageFromStream");
        BG_GET(get_w, gp_lib, "GdipGetImageWidth");
        BG_GET(get_h, gp_lib, "GdipGetImageHeight");
        BG_GET(dispose, gp_lib, "GdipDisposeImage");
        BG_GET(from_hdc, gp_lib, "GdipCreateFromHDC");
        BG_GET(draw_rect, gp_lib, "GdipDrawImageRectI");
        BG_GET(del_graphics, gp_lib, "GdipDeleteGraphics");
        BG_GET(mem_stream, sh_lib, "SHCreateMemStream");
#undef BG_GET
    }
    {
        gp_startup_input in;
        memset(&in, 0, sizeof(in));
        in.GdiplusVersion = 1;
        if (bg.startup(&bg.token, &in, NULL) != 0) { bg.failed = 1; return 0; }
    }
    bg.ready = 1;
    return 1;
}

int dsc_brand_draw_rc(void *hdc_v, int resid, int x, int y, int w, int h)
{
    HRSRC hr;
    HGLOBAL hg;
    const BYTE *data;
    DWORD size;
    IStream *stm;
    void *img = NULL;
    UINT iw = 0, ih = 0;
    int dw, dh, dx, dy;

    if (!hdc_v || w <= 0 || h <= 0) return 0;
    if (!bg_init()) return 0;

    /* 显式用宽字符版本：本文件不依赖 UNICODE 宏，RT_RCDATA 会展开成 A 版而这里调 FindResourceW */
    hr = FindResourceW(NULL, MAKEINTRESOURCEW(resid), MAKEINTRESOURCEW(10));
    if (!hr) return 0;
    hg = LoadResource(NULL, hr);
    if (!hg) return 0;
    data = (const BYTE *)LockResource(hg);
    size = SizeofResource(NULL, hr);
    if (!data || size == 0) return 0;

    stm = bg.mem_stream(data, (UINT)size);
    if (!stm) return 0;
    if (bg.load_stream(stm, &img) != 0 || !img) return 0;
    bg.get_w(img, &iw);
    bg.get_h(img, &ih);
    if (iw == 0 || ih == 0) { bg.dispose(img); return 0; }

    /* 等比缩放后在给定的框里居中 */
    dw = w;
    dh = (int)((long long)ih * w / (long long)iw);
    if (dh > h) {
        dh = h;
        dw = (int)((long long)iw * h / (long long)ih);
    }
    if (dw <= 0) dw = 1;
    if (dh <= 0) dh = 1;
    dx = x + (w - dw) / 2;
    dy = y + (h - dh) / 2;

    {
        void *g = NULL;
        if (bg.from_hdc((HDC)hdc_v, &g) == 0 && g) {
            bg.draw_rect(g, img, dx, dy, dw, dh);
            bg.del_graphics(g);
        }
    }
    bg.dispose(img);
    return 1;
}
