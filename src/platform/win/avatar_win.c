/* avatar_win.c —— GDI+ 平面 API 实现（不引入 C++，链接 -lgdiplus）。 */
#include "avatar.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "util.h"

/* ---- GDI+ 平面 API（只用需要的几个，避免 C++ 头） ---- */
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
typedef void(WINAPI *fn_shutdown)(ULONG_PTR);
typedef int(WINAPI *fn_load_from_file)(const WCHAR *, void **);
typedef int(WINAPI *fn_get_w)(void *, UINT *);
typedef int(WINAPI *fn_get_h)(void *, UINT *);
typedef int(WINAPI *fn_thumb)(void *, UINT, UINT, void **, void *, void *);
typedef int(WINAPI *fn_save)(void *, const WCHAR *, const gp_guid *, const void *);
typedef int(WINAPI *fn_dispose)(void *);
typedef int(WINAPI *fn_from_hdc)(HDC, void **);
typedef int(WINAPI *fn_draw_rect)(void *, void *, int, int, int, int);
typedef int(WINAPI *fn_del_graphics)(void *);

static struct {
    int             ready;
    int             failed;
    ULONG_PTR       token;
    fn_startup      startup;
    fn_shutdown     shutdown;
    fn_load_from_file load;
    fn_get_w        get_w;
    fn_get_h        get_h;
    fn_thumb        thumb;
    fn_save         save;
    fn_dispose      dispose;
    fn_from_hdc     from_hdc;
    fn_draw_rect    draw_rect;
    fn_del_graphics del_graphics;
} gp;

static int gp_init(void)
{
    if (gp.ready) return 1;
    if (gp.failed) return 0;
    HMODULE lib = LoadLibraryW(L"gdiplus.dll");
    if (!lib) { gp.failed = 1; return 0; }
#define GP_GET(field, name) \
    *(FARPROC *)&gp.field = GetProcAddress(lib, name); \
    if (!gp.field) { gp.failed = 1; return 0; }
    GP_GET(startup, "GdiplusStartup");
    GP_GET(shutdown, "GdiplusShutdown");
    GP_GET(load, "GdipLoadImageFromFile");
    GP_GET(get_w, "GdipGetImageWidth");
    GP_GET(get_h, "GdipGetImageHeight");
    GP_GET(thumb, "GdipGetImageThumbnail");
    GP_GET(save, "GdipSaveImageToFile");
    GP_GET(dispose, "GdipDisposeImage");
    GP_GET(from_hdc, "GdipCreateFromHDC");
    GP_GET(draw_rect, "GdipDrawImageRectI");
    GP_GET(del_graphics, "GdipDeleteGraphics");
#undef GP_GET
    gp_startup_input in;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    if (gp.startup(&gp.token, &in, NULL) != 0) { gp.failed = 1; return 0; }
    gp.ready = 1;
    return 1;
}

static void utf8_to_wide(const char *in, WCHAR *out, int cap)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
    out[cap - 1] = 0;
}

int dsc_avatar_available(void) { return gp_init(); }

int dsc_avatar_import(const char *src_path, const char *root, char *out_rel, size_t cap,
                      char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!src_path || !root || !out_rel) return DSC_E_INVAL;
    if (!gp_init()) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "系统不支持图片处理（GDI+ 不可用）");
        return DSC_E_UNSUPPORTED;
    }
    WCHAR wsrc[1024];
    utf8_to_wide(src_path, wsrc, 1024);
    void *img = NULL;
    if (gp.load(wsrc, &img) != 0 || !img) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "无法读取图片，请换一张 PNG/JPEG 图片");
        return DSC_E_FORMAT;
    }
    UINT w = 0, h = 0;
    gp.get_w(img, &w);
    gp.get_h(img, &h);
    if (w == 0 || h == 0) {
        gp.dispose(img);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "图片尺寸异常");
        return DSC_E_FORMAT;
    }
    UINT tw = w, th = h;
    if (w > DSC_AVATAR_MAX_EDGE || h > DSC_AVATAR_MAX_EDGE) {
        if (w >= h) { tw = DSC_AVATAR_MAX_EDGE; th = (UINT)((long long)h * DSC_AVATAR_MAX_EDGE / w); }
        else        { th = DSC_AVATAR_MAX_EDGE; tw = (UINT)((long long)w * DSC_AVATAR_MAX_EDGE / h); }
        if (tw == 0) tw = 1;
        if (th == 0) th = 1;
    }
    void *thumb = NULL;
    if (gp.thumb(img, tw, th, &thumb, NULL, NULL) != 0 || !thumb) {
        gp.dispose(img);
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "头像压缩失败");
        return DSC_E_IO;
    }
    char dir[1024], name[64], out[1200];
    dsc_path_join(dir, sizeof(dir), root, "avatars");
    dsc_mkdir_p(dir);
    char uuid[40];
    dsc_uuid4(uuid);
    snprintf(name, sizeof(name), "%s.jpg", uuid);
    dsc_path_join(out, sizeof(out), dir, name);

    static const gp_guid jpeg_clsid = {
        0x557CF401, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E }
    };
    WCHAR wout[1024];
    utf8_to_wide(out, wout, 1024);
    int rc = gp.save(thumb, wout, &jpeg_clsid, NULL);
    gp.dispose(thumb);
    gp.dispose(img);
    if (rc != 0) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "保存头像失败");
        return DSC_E_IO;
    }
    snprintf(out_rel, cap, "avatars/%s", name);
    return DSC_OK;
}

void dsc_avatar_remove(const char *root, const char *rel)
{
    if (!root || !rel || !rel[0]) return;
    char path[1200];
    dsc_path_join(path, sizeof(path), root, rel);
    dsc_file_remove(path);
}

int dsc_avatar_draw(void *hdc_v, const char *root, const char *rel, int x, int y, int size)
{
    if (!hdc_v || !root || !rel || !rel[0] || size <= 0) return 0;
    if (!gp_init()) return 0;
    char path[1200];
    dsc_path_join(path, sizeof(path), root, rel);
    if (!dsc_path_exists(path)) return 0;
    WCHAR wpath[1024];
    utf8_to_wide(path, wpath, 1024);
    void *img = NULL;
    if (gp.load(wpath, &img) != 0 || !img) return 0;

    HDC hdc = (HDC)hdc_v;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, size, size);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(240, 240, 242));
    RECT rc = { 0, 0, size, size };
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    void *g = NULL;
    if (gp.from_hdc(mem, &g) == 0 && g) {
        gp.draw_rect(g, img, 0, 0, size, size);
        gp.del_graphics(g);
    }
    HRGN rgn = CreateEllipticRgn(0, 0, size, size);
    int saved = SaveDC(hdc);
    SelectClipRgn(hdc, rgn);
    BitBlt(hdc, x, y, size, size, mem, 0, 0, SRCCOPY);
    RestoreDC(hdc, saved);
    DeleteObject(rgn);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    gp.dispose(img);
    return 1;
}
