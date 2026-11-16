/* GDI32 (and the DC-related USER32 calls). DCs draw into a Target: a DIB section, a DirectDraw
 * surface (IDirectDrawSurface::GetDC) or the window (presented through video.c). Text via SDL_ttf. */
#include "host.h"
#include <SDL.h>
#include <SDL_ttf.h>
#include <strings.h>

void video_set_mode(int w, int h);
void video_blit(const uint8_t *src, int pitch, int bpp, int is555, const uint32_t *pal, int sw, int sh, int dx, int dy);
void video_present(void);

typedef struct { uint32_t pixels; int w, h, pitch, bpp, is555; } Target;
typedef struct { int kind; Target t; int32_t height; int weight, italic, charset; char face[64]; TTF_Font *ttf; } GdiObj;
typedef struct { uint32_t hwnd, bitmap, font; uint32_t text_color, bk_color; int bk_mode; int has_target; Target t; } Dc;
enum { OBJ_BITMAP = 1, OBJ_FONT = 2, OBJ_STOCK = 3 };

static void tput(Target *t, int x, int y, uint32_t rgb);

static uint32_t stock[32];
API(GetStockObject, 1) {
    uint32_t i = A(0) & 31;
    if (!stock[i]) { GdiObj *o = calloc(1, sizeof *o); o->kind = OBJ_STOCK; stock[i] = handle_new(H_GDIOBJ, o); }
    return stock[i];
}

static uint32_t new_dc(uint32_t hwnd) { Dc *d = calloc(1, sizeof *d); d->hwnd = hwnd; d->bk_color = 0xFFFFFF; d->bk_mode = 2; return handle_new(H_DC, d); }
static void free_dc(uint32_t h) { Dc *d = handle_get(h, H_DC); if (d) { free(d); handle_close(h); } }
uint32_t gdi_surface_dc(uint32_t pixels, int w, int h, int pitch, int bpp, int is555) {
    uint32_t hdc = new_dc(0);
    Dc *d = handle_get(hdc, H_DC);
    d->has_target = 1; d->t = (Target){pixels, w, h, pitch, bpp, is555};
    return hdc;
}
void gdi_free_dc(uint32_t hdc) { free_dc(hdc); }

API(CreateCompatibleDC, 1) { return new_dc(0); }
API(DeleteDC, 1) { free_dc(A(0)); return 1; }
API(GetDC, 1) { return new_dc(A(0)); }
API(ReleaseDC, 2) { free_dc(A(1)); return 1; }
API(BeginPaint, 2) { memset(GPTR(A(1)), 0, 64); uint32_t h = new_dc(A(0)); WR32(A(1), h); return h; }
API(EndPaint, 2) { free_dc(RD32(A(1))); return 1; }

API(SelectObject, 2) {
    Dc *d = handle_get(A(0), H_DC); GdiObj *o = handle_get(A(1), H_GDIOBJ);
    if (!d || !o) return 0;
    uint32_t old = A(1);
    if (o->kind == OBJ_BITMAP) { old = d->bitmap; d->bitmap = A(1); d->has_target = 1; d->t = o->t; }
    else if (o->kind == OBJ_FONT) { old = d->font; d->font = A(1); }
    return old ? old : A(1);
}
API(DeleteObject, 1) {
    GdiObj *o = handle_get(A(0), H_GDIOBJ);
    if (!o || o->kind == OBJ_STOCK) return 1;
    if (o->kind == OBJ_BITMAP && o->t.pixels) gfree(o->t.pixels);
    /* fonts: o->ttf belongs to the shared cache */
    free(o); handle_close(A(0));
    return 1;
}

API(CreateDIBSection, 6) {      /* hdc, bmi, usage, bits**, section, offset */
    uint32_t bi = A(1);
    int32_t w = (int32_t)RD32(bi + 4), h = (int32_t)RD32(bi + 8);
    int bpp = RD16(bi + 14);
    int pitch = ((w * bpp + 31) / 32) * 4, ah = h < 0 ? -h : h;
    GdiObj *o = calloc(1, sizeof *o);
    o->kind = OBJ_BITMAP;
    o->t = (Target){galloc((uint32_t)(pitch * ah), 1), w, ah, pitch, bpp, 0};
    if (bpp == 16 && RD32(bi + 16) == 3 /* BI_BITFIELDS */) o->t.is555 = RD32(bi + 40 + 4) == 0x03E0;
    else if (bpp == 16) o->t.is555 = 1;          /* BI_RGB 16 bpp is 555 */
    if (h > 0) { o->t.pixels += (uint32_t)(pitch * (ah - 1)); o->t.pitch = -pitch; }   /* bottom-up: walk with negative pitch */
    if (A(3)) WR32(A(3), h > 0 ? o->t.pixels - (uint32_t)(pitch * (ah - 1)) : o->t.pixels);
    hlog("CreateDIBSection(%dx%d, %d bpp, compression %d)\n", w, h, bpp, RD32(bi + 16));
    return handle_new(H_GDIOBJ, o);
}

API(SetDIBitsToDevice, 12) {    /* hdc, xd, yd, w, h, xs, ys, start, lines, bits, bmi, usage */
    Dc *d = handle_get(A(0), H_DC);
    uint32_t bi = A(10);
    int32_t bw = (int32_t)RD32(bi + 4), bh = (int32_t)RD32(bi + 8);
    int bpp = RD16(bi + 14), pitch = ((bw * bpp + 31) / 32) * 4, ah = bh < 0 ? -bh : bh;
    int is555 = bpp == 16 && !(RD32(bi + 16) == 3 && RD32(bi + 40 + 4) == 0x07E0);
    int w = (int)A(3), h = (int)A(4), xs = (int32_t)A(5), ys = (int32_t)A(6);
    if (!d) return 0;
    if (d->has_target) {        /* copy into the DC's surface with format conversion */
        uint32_t pal = bi + RD32(bi);      /* color table follows the header (8 bpp) */
        int ncomp = RD32(bi + 16);
        int is565 = bpp == 16 && ncomp == 3 && RD32(bi + 40 + 4) == 0x07E0;
        for (int y = 0; y < h; y++) {
            int sy = ys + y, ty = (int32_t)A(2) + y;
            if (sy < 0 || sy >= ah || ty < 0 || ty >= d->t.h) continue;
            int row = bh > 0 ? ah - 1 - sy : sy;
            const uint8_t *sp = (const uint8_t *)GPTR(A(9)) + (size_t)row * pitch;
            for (int x = 0; x < w; x++) {
                int sx = xs + x, tx = (int32_t)A(1) + x;
                if (sx < 0 || sx >= bw || tx < 0 || tx >= d->t.w) continue;
                uint32_t rgb;
                switch (bpp) {
                case 8: { uint32_t e = pal + 4u * sp[sx]; rgb = (uint32_t)RD8(e + 2) << 16 | RD8(e + 1) << 8 | RD8(e); break; }
                case 16: { uint16_t v = (uint16_t)(sp[2 * sx] | sp[2 * sx + 1] << 8); uint32_t r, g, b;
                    if (is565) { r = v >> 11; g = (v >> 5) & 63; b = v & 31; r = r << 3 | r >> 2; g = g << 2 | g >> 4; }
                    else { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = r << 3 | r >> 2; g = g << 3 | g >> 2; }
                    rgb = r << 16 | g << 8 | (b << 3 | b >> 2); break; }
                case 24: rgb = (uint32_t)sp[3 * sx + 2] << 16 | sp[3 * sx + 1] << 8 | sp[3 * sx]; break;
                default: rgb = (uint32_t)sp[4 * sx + 2] << 16 | sp[4 * sx + 1] << 8 | sp[4 * sx]; break;
                }
                tput(&d->t, tx, ty, rgb);
            }
        }
        return A(8);
    }
    video_set_mode(800, 600);
    { static int n; if (n++ < 3) hlog("SetDIBitsToDevice(screen) %dx%d at %d,%d bpp %d is555 %d\n", w, h, (int32_t)A(1), (int32_t)A(2), bpp, is555); }
    /* top-down view of the source rows */
    for (int y = 0; y < h; y++) {
        int sy = ys + y; if (sy < 0 || sy >= ah) continue;
        int row = bh > 0 ? ah - 1 - sy : sy;
        video_blit((uint8_t *)GPTR(A(9)) + (size_t)row * pitch + (size_t)xs * (bpp / 8), pitch, bpp, is555, NULL, w, 1,
                   (int32_t)A(1), (int32_t)A(2) + y);
    }
    video_present();
    return A(8);
}

API(GetDeviceCaps, 2) {
    switch (A(1)) {
    case 8: return 800; case 10: return 600;         /* HORZRES, VERTRES (fullscreen mode) */
    case 12: return 16; case 14: return 1;           /* BITSPIXEL, PLANES */
    case 24: return 0xFFFFFFFFu;                     /* NUMCOLORS */
    case 38: return 0x7E99;                          /* RASTERCAPS */
    case 88: case 90: return 96;                     /* LOGPIXELSX/Y */
    case 104: return 0;                              /* SIZEPALETTE */
    case 116: return 60;                             /* VREFRESH */
    default: return 0;
    }
}
API(GetSystemPaletteEntries, 4) { if (A(3)) memset(GPTR(A(3)), 0, 4 * A(2)); return 0; }

/* ---------------------------------------------------------------- fonts & text */
static const char *font_file_lookup(int japanese) {
    const char *e = getenv(japanese ? "JOEY_FONT_JP" : "JOEY_FONT");
    if (e) return e;
    static char p[2][1100];
    snprintf(p[japanese], sizeof p[japanese], "%s/fonts/%s", game_dir, japanese ? "jp.ttf" : "regular.ttf");     /* user override */
    FILE *f = fopen(p[japanese], "rb"); if (f) { fclose(f); return p[japanese]; }
#ifdef __vita__
    return "app0:fonts/Arimo.ttf";      /* bundled in the VPK; metric-compatible with Arial */
#elif defined(_WIN32)
    if (japanese) return "C:\\Windows\\Fonts\\msgothic.ttc";
    char *base = SDL_GetBasePath();                    /* fonts\Arimo.ttf next to Joey.exe */
    if (base) {
        snprintf(p[0], sizeof p[0], "%sfonts\\Arimo.ttf", base); SDL_free(base);
        f = fopen(p[0], "rb"); if (f) { fclose(f); return p[0]; }
    }
    return "C:\\Windows\\Fonts\\arial.ttf";
#elif defined(__ANDROID__)
    if (japanese) return "/system/fonts/NotoSansCJK-Regular.ttc";
    return "fonts/Arimo.ttf";           /* APK asset: SDL_RWFromFile reads relative paths from the APK */
#else
    if (japanese) return "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc";
    f = fopen("assets/fonts/Arimo.ttf", "rb"); if (f) { fclose(f); return "assets/fonts/Arimo.ttf"; }
    char *base = SDL_GetBasePath();                    /* Joey.app: Contents/Resources/fonts */
    if (base) {
        snprintf(p[0], sizeof p[0], "%sfonts/Arimo.ttf", base); SDL_free(base);
        f = fopen(p[0], "rb"); if (f) { fclose(f); return p[0]; }
    }
    return "/System/Library/Fonts/Supplemental/Arial.ttf";
#endif
}
static const char *font_file(int japanese) {
    static const char *cached[2];
    if (!cached[japanese]) cached[japanese] = font_file_lookup(japanese);
    return cached[japanese];
}
/* The game creates and deletes fonts constantly (every card hover / turn banner): opening a TTF is
 * expensive (file I/O + FreeType setup + empty glyph cache), so open each (file, size, style) once
 * and share it between font objects for the whole run. */
static TTF_Font *ttf_cached(int jp, int px, int style) {
    static struct { int jp, px, style; TTF_Font *f; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].jp == jp && cache[i].px == px && cache[i].style == style) return cache[i].f;
    TTF_Font *f = TTF_OpenFont(font_file(jp), px);
    if (!f) { hlog("TTF_OpenFont(%s): %s\n", font_file(jp), TTF_GetError()); return NULL; }
    if (style) TTF_SetFontStyle(f, style);
    hlog("font: opened %s size %d style %d\n", font_file(jp), px, style);
    if (n < 64) { cache[n].jp = jp; cache[n].px = px; cache[n].style = style; cache[n].f = f; n++; }
    return f;
}
static TTF_Font *ttf_of(GdiObj *f) {
    if (f->ttf) return f->ttf;
    if (!TTF_WasInit() && TTF_Init() != 0) { hlog("TTF_Init: %s\n", TTF_GetError()); return NULL; }
    int jp = f->charset == 128;
    for (const unsigned char *c = (const unsigned char *)f->face; *c; c++) if (*c >= 0x80) jp = 1;
    int px = f->height < 0 ? -f->height : (f->height ? f->height * 85 / 100 : 13);
    int host_small_screen(void);
    if (px <= 12 && host_small_screen()) px++;   /* small text (card descriptions, 11px) is barely legible once 800x600
                                                    is scaled to a handheld screen: Vita, phones (touchui.c) */
    int style = f->weight >= 600 ? TTF_STYLE_BOLD | (f->italic ? TTF_STYLE_ITALIC : 0) : 0;
    return f->ttf = ttf_cached(jp, px, style);
}
static GdiObj *dc_font(Dc *d) {
    GdiObj *f = d ? handle_get(d->font, H_GDIOBJ) : NULL;
    if (f && f->kind == OBJ_FONT) return f;
    static GdiObj sysfont = {OBJ_FONT, {0}, -13, 400, 0, 0, "", NULL};
    return &sysfont;
}
API(CreateFontA, 14) {          /* height, width, esc, orient, weight, italic, underline, strike, charset, outprec, clipprec, quality, pitch, face */
    GdiObj *o = calloc(1, sizeof *o);
    o->kind = OBJ_FONT; o->height = (int32_t)A(0); o->weight = (int)A(4); o->italic = A(5) != 0; o->charset = (int)(A(8) & 0xFF);
    snprintf(o->face, sizeof o->face, "%s", A(13) ? GSTR(A(13)) : "");
    TRACE_API("CreateFontA(height %d, weight %d, charset %d, face \"%s\")\n", o->height, o->weight, o->charset, o->face);
    return handle_new(H_GDIOBJ, o);
}
API(SetBkMode, 2) { Dc *d = handle_get(A(0), H_DC); if (!d) return 0; int o = d->bk_mode; d->bk_mode = (int)A(1); return (uint32_t)o; }
API(SetBkColor, 2) { Dc *d = handle_get(A(0), H_DC); if (!d) return 0xFFFFFFFFu; uint32_t o = d->bk_color; d->bk_color = A(1); return o; }
API(SetTextColor, 2) { Dc *d = handle_get(A(0), H_DC); if (!d) return 0xFFFFFFFFu; uint32_t o = d->text_color; d->text_color = A(1); return o; }

static uint16_t *wstr(uint32_t p, uint32_t n) {
    uint16_t *s = malloc(2 * (n + 1));
    for (uint32_t i = 0; i < n; i++) s[i] = RD16(p + 2 * i);
    s[n] = 0;
    return s;
}
API_PROF(GetTextExtentPoint32W, 4, PROF_TEXT) { /* hdc, str, n, SIZE* */
    TTF_Font *f = ttf_of(dc_font(handle_get(A(0), H_DC)));
    int w = 0, h = 0;
    if (f) { uint16_t *s = wstr(A(1), A(2)); TTF_SizeUNICODE(f, s, &w, &h); free(s); }
    WR32(A(3), (uint32_t)w); WR32(A(3) + 4, (uint32_t)(f ? TTF_FontHeight(f) : h));
    return 1;
}

/* read/write one pixel of a target as RGB */
static uint32_t tget(Target *t, int x, int y) {
    uint8_t *p = (uint8_t *)GPTR(t->pixels) + (ptrdiff_t)y * t->pitch;
    switch (t->bpp) {
    case 16: { uint16_t v = (uint16_t)(p[2 * x] | p[2 * x + 1] << 8); uint32_t r, g, b;
        if (t->is555) { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = r << 3 | r >> 2; g = g << 3 | g >> 2; }
        else { r = v >> 11; g = (v >> 5) & 63; b = v & 31; r = r << 3 | r >> 2; g = g << 2 | g >> 4; }
        return r << 16 | g << 8 | (b << 3 | b >> 2); }
    case 24: return (uint32_t)p[3 * x + 2] << 16 | p[3 * x + 1] << 8 | p[3 * x];
    case 32: return (uint32_t)p[4 * x + 2] << 16 | p[4 * x + 1] << 8 | p[4 * x];
    }
    return 0;
}
static void tput(Target *t, int x, int y, uint32_t rgb) {
    uint8_t *p = (uint8_t *)GPTR(t->pixels) + (ptrdiff_t)y * t->pitch;
    uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    switch (t->bpp) {
    case 16: { uint16_t v = t->is555 ? (uint16_t)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3) : (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        p[2 * x] = (uint8_t)v; p[2 * x + 1] = (uint8_t)(v >> 8); break; }
    case 24: p[3 * x] = (uint8_t)b; p[3 * x + 1] = (uint8_t)g; p[3 * x + 2] = (uint8_t)r; break;
    case 32: p[4 * x] = (uint8_t)b; p[4 * x + 1] = (uint8_t)g; p[4 * x + 2] = (uint8_t)r; break;
    }
}
static uint32_t colorref_rgb(uint32_t c) { return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }

API_PROF(TextOutW, 5, PROF_TEXT) {              /* hdc, x, y, str, n */
    Dc *d = handle_get(A(0), H_DC);
    if (!d || !d->has_target || !A(4)) return 1;
    TTF_Font *f = ttf_of(dc_font(d));
    if (!f) return 1;
    uint16_t *s = wstr(A(3), A(4));
    if (getenv("JOEY_TRACE_TEXT")) { char a[128]; int i = 0; for (; s[i] && i < 127; i++) a[i] = s[i] < 128 ? (char)s[i] : '?'; a[i] = 0;
        hlog("TextOutW(\"%s\", n %u) from 0x%x\n", a, A(4), RD32(ctx->esp)); }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *g = TTF_RenderUNICODE_Blended(f, s, white);
    free(s);
    if (!g) return 1;
    SDL_Surface *a = SDL_ConvertSurfaceFormat(g, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(g);
    Target *t = &d->t;
    int x0 = (int32_t)A(1), y0 = (int32_t)A(2);
    uint32_t fg = colorref_rgb(d->text_color), bg = colorref_rgb(d->bk_color);
    for (int y = 0; y < a->h; y++) {
        int ty = y0 + y; if (ty < 0 || ty >= t->h) continue;
        const uint32_t *row = (const uint32_t *)((uint8_t *)a->pixels + (size_t)y * a->pitch);
        for (int x = 0; x < a->w; x++) {
            int tx = x0 + x; if (tx < 0 || tx >= t->w) continue;
            uint32_t al = row[x] >> 24;
            uint32_t under = d->bk_mode == 2 ? bg : (al ? tget(t, tx, ty) : 0);
            if (!al && d->bk_mode != 2) continue;
            uint32_t r = ((fg >> 16 & 255) * al + (under >> 16 & 255) * (255 - al)) / 255;
            uint32_t gg = ((fg >> 8 & 255) * al + (under >> 8 & 255) * (255 - al)) / 255;
            uint32_t b = ((fg & 255) * al + (under & 255) * (255 - al)) / 255;
            tput(t, tx, ty, r << 16 | gg << 8 | b);
        }
    }
    SDL_FreeSurface(a);
    return 1;
}

const HostExport gdi32_exports[] = {
#define X(n) {#n, w_##n},
    X(GetStockObject) X(CreateCompatibleDC) X(DeleteDC) X(SelectObject) X(DeleteObject) X(CreateDIBSection)
    X(SetDIBitsToDevice) X(GetDeviceCaps) X(GetSystemPaletteEntries) X(CreateFontA) X(SetBkMode) X(SetBkColor)
    X(SetTextColor) X(GetTextExtentPoint32W) X(TextOutW)
#undef X
    {NULL, NULL}};
const HostExport user32_gdi_exports[] = {
#define X(n) {#n, w_##n},
    X(GetDC) X(ReleaseDC) X(BeginPaint) X(EndPaint)
#undef X
    {NULL, NULL}};
