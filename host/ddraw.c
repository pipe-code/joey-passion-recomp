/* DirectDraw (interfaces of DirectX 1: IDirectDraw, IDirectDrawSurface, IDirectDrawClipper, IDirectDrawPalette). */
#include "host.h"

void video_set_mode(int w, int h);
void video_blit(const uint8_t *src, int pitch, int bpp, int is555, const uint32_t *pal, int sw, int sh, int dx, int dy);
void video_present(void);
void video_present_surface(const uint8_t *src, int pitch, int bpp, int is555, const uint32_t *pal, int w, int h);
void host_client_origin(int *x, int *y);

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define DDERR_NOTFOUND 0x887600FFu

#define DDSCL_FULLSCREEN 0x1
#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_CKSRCBLT 0x10000

typedef struct { int refs; uint32_t hwnd, coop; int mw, mh, mbpp; uint32_t primary; } DDraw;
typedef struct { int refs; uint32_t entries[256]; } DDPal;            /* ARGB */
typedef struct { int refs; uint32_t hwnd; } DDClip;
typedef struct Surf {
    int refs; DDraw *dd; uint32_t self, caps;
    int w, h, bpp, pitch, is555;
    uint32_t pixels, back;                /* back: attached back buffer surface (guest COM ptr) */
    int has_ck; uint32_t ck_lo, ck_hi;
    uint32_t clipper, palette;
} Surf;

static uint32_t vt_dd, vt_surf, vt_clip, vt_pal;
static void vtables(void);

static int guid_is(uint32_t p, uint32_t d1) { return p && RD32(p) == d1; }

/* ---------------------------------------------------------------- pixel formats */
static void write_pf(uint32_t pf, int bpp, int is555) {
    memset(GPTR(pf), 0, 32);
    WR32(pf, 32);
    if (bpp == 8) { WR32(pf + 4, 0x40 | 0x20); WR32(pf + 12, 8); return; }      /* DDPF_RGB | PALETTEINDEXED8 */
    WR32(pf + 4, 0x40); WR32(pf + 12, bpp);
    if (bpp == 16 && is555) { WR32(pf + 16, 0x7C00); WR32(pf + 20, 0x03E0); WR32(pf + 24, 0x001F); }
    else if (bpp == 16) { WR32(pf + 16, 0xF800); WR32(pf + 20, 0x07E0); WR32(pf + 24, 0x001F); }
    else { WR32(pf + 16, 0xFF0000); WR32(pf + 20, 0x00FF00); WR32(pf + 24, 0x0000FF); }
}
static void write_desc(uint32_t d, Surf *s, uint32_t lp) {
    uint32_t size = RD32(d) ? RD32(d) : 108;
    memset(GPTR(d), 0, size < 108 ? size : 108);
    WR32(d, size);
    WR32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (lp ? DDSD_LPSURFACE : 0) | (s->has_ck ? DDSD_CKSRCBLT : 0));
    WR32(d + 8, s->h); WR32(d + 12, s->w); WR32(d + 16, s->pitch);
    WR32(d + 20, s->back ? 1 : 0);
    WR32(d + 36, lp);
    WR32(d + 64, s->ck_lo); WR32(d + 68, s->ck_hi);
    write_pf(d + 72, s->bpp, s->is555);
    WR32(d + 104, s->caps);
}

/* ---------------------------------------------------------------- presentation */
static void present_surface(Surf *s) {
    DDraw *dd = s->dd;
    if (!dd) return;
    const uint32_t *pal = NULL;
    DDPal *p = s->palette ? com_host(s->palette) : NULL;
    if (p) pal = p->entries;
    int ox = 0, oy = 0;
    if (!(dd->coop & DDSCL_FULLSCREEN)) host_client_origin(&ox, &oy);     /* windowed: primary = desktop */
    video_set_mode(dd->mw ? dd->mw : 800, dd->mh ? dd->mh : 600);
    int w = dd->mw ? dd->mw : 800, h = dd->mh ? dd->mh : 600;
    if (ox + w > s->w) w = s->w - ox;
    if (oy + h > s->h) h = s->h - oy;
    if (w <= 0 || h <= 0) return;
    video_present_surface((uint8_t *)GPTR(s->pixels) + (size_t)oy * s->pitch + (size_t)ox * (s->bpp / 8), s->pitch, s->bpp, s->is555, pal, w, h);
}

/* ---------------------------------------------------------------- IUnknown helpers */
#define REFS(T)                                                                          \
    API(T##_AddRef, 1) { T *o = THIS_HOST(T); return (uint32_t)++o->refs; }             \
    API(T##_Release, 1) { T *o = THIS_HOST(T); return (uint32_t)(o->refs > 0 ? --o->refs : 0); }
REFS(DDraw)
REFS(DDClip)
REFS(DDPal)

/* ---------------------------------------------------------------- IDirectDraw */
static int mode_bpp(DDraw *dd) { return dd->mbpp ? dd->mbpp : 32; }

API(DDraw_QueryInterface, 3) {
    if (guid_is(A(1), 0x6C14DB80u) || guid_is(A(1), 0)) { WR32(A(2), A(0)); THIS_HOST(DDraw)->refs++; return DD_OK; }
    hlog("IDirectDraw::QueryInterface(%08x-...) -> E_NOINTERFACE\n", RD32(A(1)));
    WR32(A(2), 0); return E_NOINTERFACE;
}
API(DDraw_Initialize, 2) { return DD_OK; }
API(DDraw_SetCooperativeLevel, 3) {
    DDraw *dd = THIS_HOST(DDraw); dd->hwnd = A(1); dd->coop = A(2);
    hlog("IDirectDraw::SetCooperativeLevel(hwnd 0x%x, flags 0x%x)\n", A(1), A(2));
    void host_set_fullscreen(int on); host_set_fullscreen((A(2) & DDSCL_FULLSCREEN) != 0);
    return DD_OK;
}
API(DDraw_SetDisplayMode, 4) {
    DDraw *dd = THIS_HOST(DDraw); dd->mw = A(1); dd->mh = A(2); dd->mbpp = A(3);
    hlog("IDirectDraw::SetDisplayMode(%dx%dx%d)\n", A(1), A(2), A(3));
    video_set_mode(dd->mw, dd->mh);
    return DD_OK;
}
API(DDraw_RestoreDisplayMode, 1) { DDraw *dd = THIS_HOST(DDraw); dd->mw = dd->mh = dd->mbpp = 0; return DD_OK; }
static void fill_mode_desc(uint32_t d, int w, int h, int bpp) {
    memset(GPTR(d), 0, 108); WR32(d, 108);
    WR32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | 0x20000 /* REFRESHRATE */);
    WR32(d + 8, h); WR32(d + 12, w); WR32(d + 16, w * bpp / 8); WR32(d + 24, 60);
    write_pf(d + 72, bpp, 0);
}
API(DDraw_GetDisplayMode, 2) {
    DDraw *dd = THIS_HOST(DDraw);
    fill_mode_desc(A(1), dd->mw ? dd->mw : 1024, dd->mh ? dd->mh : 768, mode_bpp(dd));
    return DD_OK;
}
API(DDraw_EnumDisplayModes, 5) {    /* flags, desc filter, ctx, callback(desc*, ctx) */
    static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}};
    static const int bpps[] = {8, 16, 32};
    uint32_t d = galloc(108, 1);
    for (int b = 0; b < 3; b++) for (int m = 0; m < 3; m++) {
        fill_mode_desc(d, modes[m][0], modes[m][1], bpps[b]);
        if (!guest_call(ctx, A(4), 2, d, A(3))) { gfree(d); return DD_OK; }
    }
    gfree(d);
    return DD_OK;
}
API(DDraw_GetCaps, 3) {
    for (int i = 1; i <= 2; i++) {
        uint32_t c = A(i); if (!c) continue;
        uint32_t size = RD32(c); memset(GPTR(c + 4), 0, size > 4 ? size - 4 : 0);
        WR32(c + 4, 0x40 | 0x200 | 0x40000 | 0x4000000);       /* BLT, BLTSTRETCH, COLORKEY, BLTCOLORFILL */
        WR32(c + 12, 0x200);                                   /* DDCKEYCAPS_SRCBLT */
        if (size >= 68) { WR32(c + 60, 64u << 20); WR32(c + 64, 60u << 20); }
    }
    return DD_OK;
}
API(DDraw_WaitForVerticalBlank, 3) { return DD_OK; }
API(DDraw_FlipToGDISurface, 1) { return DD_OK; }
API(DDraw_GetMonitorFrequency, 2) { WR32(A(1), 60); return DD_OK; }

static uint32_t new_surface(DDraw *dd, int w, int h, int bpp, uint32_t caps) {
    Surf *s = calloc(1, sizeof *s);
    s->refs = 1; s->dd = dd; s->caps = caps; s->w = w; s->h = h; s->bpp = bpp;
    s->pitch = ((w * bpp / 8) + 3) & ~3;
    s->pixels = galloc((uint32_t)s->pitch * h, 1);
    s->self = com_new(vt_surf, s);
    return s->self;
}
API(DDraw_CreateSurface, 4) {       /* desc, lplpSurface, outer */
    DDraw *dd = THIS_HOST(DDraw);
    uint32_t d = A(1), fl = RD32(d + 4), caps = (fl & DDSD_CAPS) ? RD32(d + 104) : 0;
    int bpp = mode_bpp(dd), is555 = 0;
    if (fl & DDSD_PIXELFORMAT) {
        bpp = RD32(d + 72 + 12);
        is555 = bpp == 16 && RD32(d + 72 + 20) == 0x03E0;
    }
    int w, h;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = dd->mw ? dd->mw : 1024; h = dd->mh ? dd->mh : 768; bpp = mode_bpp(dd);
    } else { w = RD32(d + 12); h = RD32(d + 8); }
    uint32_t s = new_surface(dd, w, h, bpp, caps | ((caps & DDSCAPS_PRIMARYSURFACE) ? DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY : 0));
    Surf *ss = com_host(s); ss->is555 = is555;
    if (fl & DDSD_CKSRCBLT) { ss->has_ck = 1; ss->ck_lo = RD32(d + 64); ss->ck_hi = RD32(d + 68); }
    if ((caps & DDSCAPS_PRIMARYSURFACE) && (fl & DDSD_BACKBUFFERCOUNT) && RD32(d + 20)) {
        ss->back = new_surface(dd, w, h, bpp, DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY | (caps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX)));
        ((Surf *)com_host(ss->back))->is555 = is555;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) dd->primary = s;
    hlog("IDirectDraw::CreateSurface(%dx%dx%d caps 0x%x flags 0x%x%s) -> 0x%x\n", w, h, bpp, caps, fl, ss->back ? " +backbuffer" : "", s);
    WR32(A(2), s);
    return DD_OK;
}
API(DDraw_CreateClipper, 4) { DDClip *c = calloc(1, sizeof *c); c->refs = 1; WR32(A(2), com_new(vt_clip, c)); return DD_OK; }
API(DDraw_CreatePalette, 5) {       /* flags, entries (PALETTEENTRY r,g,b,flags), lplpPal, outer */
    DDPal *p = calloc(1, sizeof *p); p->refs = 1;
    for (int i = 0; i < 256 && A(2); i++) p->entries[i] = 0xFF000000u | RD8(A(2) + 4 * i) << 16 | RD8(A(2) + 4 * i + 1) << 8 | RD8(A(2) + 4 * i + 2);
    WR32(A(3), com_new(vt_pal, p));
    return DD_OK;
}

/* ---------------------------------------------------------------- IDirectDrawSurface */
API(Surf_QueryInterface, 3) {
    if (guid_is(A(1), 0x6C14DB81u) || guid_is(A(1), 0)) { WR32(A(2), A(0)); THIS_HOST(Surf)->refs++; return DD_OK; }
    hlog("IDirectDrawSurface::QueryInterface(%08x-...) -> E_NOINTERFACE\n", RD32(A(1)));
    WR32(A(2), 0); return E_NOINTERFACE;
}
API(Surf_GetSurfaceDesc, 2) { write_desc(A(1), THIS_HOST(Surf), 0); return DD_OK; }
API(Surf_GetPixelFormat, 2) { Surf *s = THIS_HOST(Surf); write_pf(A(1), s->bpp, s->is555); return DD_OK; }
API(Surf_GetCaps, 2) { WR32(A(1), THIS_HOST(Surf)->caps); return DD_OK; }
API(Surf_IsLost, 1) { return DD_OK; }
API(Surf_Restore, 1) { return DD_OK; }
API(Surf_GetBltStatus, 2) { return DD_OK; }
API(Surf_GetFlipStatus, 2) { return DD_OK; }
API(Surf_SetClipper, 2) { THIS_HOST(Surf)->clipper = A(1); return DD_OK; }
API(Surf_SetPalette, 2) { THIS_HOST(Surf)->palette = A(1); return DD_OK; }
API(Surf_GetPalette, 2) { Surf *s = THIS_HOST(Surf); WR32(A(1), s->palette); return s->palette ? DD_OK : DDERR_NOTFOUND; }
API(Surf_SetColorKey, 3) {          /* flags, DDCOLORKEY* */
    Surf *s = THIS_HOST(Surf);
    if (A(2)) { s->has_ck = 1; s->ck_lo = RD32(A(2)); s->ck_hi = RD32(A(2) + 4); } else s->has_ck = 0;
    return DD_OK;
}
API(Surf_GetColorKey, 3) { Surf *s = THIS_HOST(Surf); if (!s->has_ck) return DDERR_NOTFOUND; WR32(A(2), s->ck_lo); WR32(A(2) + 4, s->ck_hi); return DD_OK; }
/* Release frees the surface when the last reference goes (the game creates and drops sprite and card
 * surfaces all the time; leaking them exhausted the Vita's guest heap after a duel or two). */
static void surf_destroy(Surf *s) {
    if (s->back) {                  /* a complex primary owns its back buffer */
        Surf *b = com_host(s->back);
        if (b && --b->refs <= 0) surf_destroy(b);
        s->back = 0;
    }
    /* the primary's pixels may still be read by the render thread: keep them (one surface, mode changes only) */
    if (s->dd && s->dd->primary == s->self) s->dd->primary = 0;
    else if (!(s->caps & DDSCAPS_PRIMARYSURFACE)) gfree(s->pixels);
    com_free(s->self);
    free(s);
}
API(Surf_AddRef, 1) { return (uint32_t)++THIS_HOST(Surf)->refs; }
API(Surf_Release, 1) {
    Surf *s = THIS_HOST(Surf);
    if (!s) return 0;
    if (--s->refs > 0) return (uint32_t)s->refs;
    surf_destroy(s);
    return 0;
}
API(Surf_GetAttachedSurface, 3) {   /* DDSCAPS*, lplp */
    Surf *s = THIS_HOST(Surf);
    if ((RD32(A(1)) & DDSCAPS_BACKBUFFER) && s->back) { ((Surf *)com_host(s->back))->refs++; WR32(A(2), s->back); return DD_OK; }
    WR32(A(2), 0); return DDERR_NOTFOUND;
}
API(Surf_Lock, 5) {                 /* rect, desc, flags, event */
    Surf *s = THIS_HOST(Surf);
    uint32_t p = s->pixels;
    if (A(1)) p += (uint32_t)((int32_t)RD32(A(1) + 4) * s->pitch + (int32_t)RD32(A(1)) * (s->bpp / 8));
    write_desc(A(2), s, p);
    return DD_OK;
}
API(Surf_Unlock, 2) { Surf *s = THIS_HOST(Surf); if (s->caps & DDSCAPS_PRIMARYSURFACE) present_surface(s); return DD_OK; }
API(Surf_Flip, 3) {
    Surf *s = THIS_HOST(Surf);
    if (s->back) {
        Surf *b = com_host(s->back);
        uint32_t t = s->pixels; s->pixels = b->pixels; b->pixels = t;
    }
    present_surface(s);
    return DD_OK;
}

static uint32_t get_px(Surf *s, int x, int y) {
    uint8_t *p = (uint8_t *)GPTR(s->pixels) + (size_t)y * s->pitch;
    switch (s->bpp) { case 8: return p[x]; case 16: return p[2 * x] | p[2 * x + 1] << 8;
                      case 24: return p[3 * x] | p[3 * x + 1] << 8 | (uint32_t)p[3 * x + 2] << 16;
                      default: { uint32_t v; memcpy(&v, p + 4 * x, 4); return v; } }
}
static void put_px(Surf *s, int x, int y, uint32_t c) {
    uint8_t *p = (uint8_t *)GPTR(s->pixels) + (size_t)y * s->pitch;
    switch (s->bpp) { case 8: p[x] = (uint8_t)c; break; case 16: p[2 * x] = (uint8_t)c; p[2 * x + 1] = (uint8_t)(c >> 8); break;
                      case 24: p[3 * x] = (uint8_t)c; p[3 * x + 1] = (uint8_t)(c >> 8); p[3 * x + 2] = (uint8_t)(c >> 16); break;
                      default: memcpy(p + 4 * x, &c, 4); }
}
static void read_rect(uint32_t r, Surf *s, int *l, int *t, int *rr, int *b) {
    if (r) { *l = (int32_t)RD32(r); *t = (int32_t)RD32(r + 4); *rr = (int32_t)RD32(r + 8); *b = (int32_t)RD32(r + 12); }
    else { *l = 0; *t = 0; *rr = s->w; *b = s->h; }
}
/* generic blit with optional stretch (nearest) and source color key; clipped to destination */
static void do_blit(Surf *d, int dl, int dt, int dr, int db, Surf *s, int sl, int st, int sr, int sb, int key) {
    int dw = dr - dl, dh = db - dt, sw = sr - sl, sh = sb - st;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int fast = (dw == sw && dh == sh && !key && d->bpp == s->bpp);
    for (int y = 0; y < dh; y++) {
        int ty = dt + y; if (ty < 0 || ty >= d->h) continue;
        int sy = st + (int)((int64_t)y * sh / dh); if (sy < 0 || sy >= s->h) continue;
        if (fast && dl >= 0 && dr <= d->w && sl >= 0 && sr <= s->w) {
            int bpp = d->bpp / 8;
            memmove((uint8_t *)GPTR(d->pixels) + (size_t)ty * d->pitch + (size_t)dl * bpp,
                    (uint8_t *)GPTR(s->pixels) + (size_t)sy * s->pitch + (size_t)sl * bpp, (size_t)dw * bpp);
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int tx = dl + x; if (tx < 0 || tx >= d->w) continue;
            int sx = sl + (int)((int64_t)x * sw / dw); if (sx < 0 || sx >= s->w) continue;
            uint32_t c = get_px(s, sx, sy);
            if (key && c >= s->ck_lo && c <= s->ck_hi) continue;
            put_px(d, tx, ty, c);
        }
    }
}
API(Surf_Blt, 6) {                  /* dstrect, src, srcrect, flags, fx */
    Surf *d = THIS_HOST(Surf);
    uint32_t fl = A(4);
    int dl, dt, dr, db; read_rect(A(1), d, &dl, &dt, &dr, &db);
    if (fl & 0x400) {               /* DDBLT_COLORFILL */
        uint32_t c = RD32(A(5) + 80);
        for (int y = dt; y < db; y++) if (y >= 0 && y < d->h) for (int x = dl; x < dr; x++) if (x >= 0 && x < d->w) put_px(d, x, y, c);
    } else if (A(2)) {
        Surf *s = com_host(A(2));
        int sl, st, sr, sb; read_rect(A(3), s, &sl, &st, &sr, &sb);
        do_blit(d, dl, dt, dr, db, s, sl, st, sr, sb, (fl & 0x8000) && s->has_ck);
    } else hlog("IDirectDrawSurface::Blt flags 0x%x not handled\n", fl);
    if (d->caps & DDSCAPS_PRIMARYSURFACE) present_surface(d);
    return DD_OK;
}
API(Surf_BltFast, 6) {              /* x, y, src, srcrect, trans */
    Surf *d = THIS_HOST(Surf), *s = com_host(A(3));
    int sl, st, sr, sb; read_rect(A(4), s, &sl, &st, &sr, &sb);
    int x = (int32_t)A(1), y = (int32_t)A(2);
    do_blit(d, x, y, x + sr - sl, y + sb - st, s, sl, st, sr, sb, (A(5) & 1) && s->has_ck);
    if (d->caps & DDSCAPS_PRIMARYSURFACE) present_surface(d);
    return DD_OK;
}

uint32_t gdi_surface_dc(uint32_t pixels, int w, int h, int pitch, int bpp, int is555);
void gdi_free_dc(uint32_t hdc);
API(Surf_GetDC, 2) { Surf *s = THIS_HOST(Surf); WR32(A(1), gdi_surface_dc(s->pixels, s->w, s->h, s->pitch, s->bpp, s->is555)); return DD_OK; }
API(Surf_ReleaseDC, 2) { Surf *s = THIS_HOST(Surf); gdi_free_dc(A(1)); if (s->caps & DDSCAPS_PRIMARYSURFACE) present_surface(s); return DD_OK; }

/* ---------------------------------------------------------------- IDirectDrawClipper / Palette */
API(DDClip_SetHWnd, 3) { THIS_HOST(DDClip)->hwnd = A(2); return DD_OK; }
API(DDClip_GetHWnd, 2) { WR32(A(1), THIS_HOST(DDClip)->hwnd); return DD_OK; }
API(DDPal_SetEntries, 5) {          /* flags, start, count, entries */
    DDPal *p = THIS_HOST(DDPal);
    for (uint32_t i = 0; i < A(3) && A(2) + i < 256; i++)
        p->entries[A(2) + i] = 0xFF000000u | RD8(A(4) + 4 * i) << 16 | RD8(A(4) + 4 * i + 1) << 8 | RD8(A(4) + 4 * i + 2);
    return DD_OK;
}
API(DDPal_GetEntries, 5) {
    DDPal *p = THIS_HOST(DDPal);
    for (uint32_t i = 0; i < A(3) && A(2) + i < 256; i++) {
        uint32_t c = p->entries[A(2) + i];
        WR8(A(4) + 4 * i, (uint8_t)(c >> 16)); WR8(A(4) + 4 * i + 1, (uint8_t)(c >> 8)); WR8(A(4) + 4 * i + 2, (uint8_t)c); WR8(A(4) + 4 * i + 3, 0);
    }
    return DD_OK;
}

/* ---------------------------------------------------------------- vtables & creation */
#define M(n) {#n, NULL}
#define I(T, n) {#n, w_##T##_##n}
static void vtables(void) {
    if (vt_dd) return;
    static const ComMethod dd[] = {
        I(DDraw, QueryInterface), I(DDraw, AddRef), I(DDraw, Release), M(Compact), I(DDraw, CreateClipper),
        I(DDraw, CreatePalette), I(DDraw, CreateSurface), M(DuplicateSurface), I(DDraw, EnumDisplayModes),
        M(EnumSurfaces), I(DDraw, FlipToGDISurface), I(DDraw, GetCaps), I(DDraw, GetDisplayMode), M(GetFourCCCodes),
        M(GetGDISurface), I(DDraw, GetMonitorFrequency), M(GetScanLine), M(GetVerticalBlankStatus), I(DDraw, Initialize),
        I(DDraw, RestoreDisplayMode), I(DDraw, SetCooperativeLevel), I(DDraw, SetDisplayMode), I(DDraw, WaitForVerticalBlank)};
    static const ComMethod sf[] = {
        I(Surf, QueryInterface), I(Surf, AddRef), I(Surf, Release), M(AddAttachedSurface), M(AddOverlayDirtyRect),
        I(Surf, Blt), M(BltBatch), I(Surf, BltFast), M(DeleteAttachedSurface), M(EnumAttachedSurfaces),
        M(EnumOverlayZOrders), I(Surf, Flip), I(Surf, GetAttachedSurface), I(Surf, GetBltStatus), I(Surf, GetCaps),
        M(GetClipper), I(Surf, GetColorKey), I(Surf, GetDC), I(Surf, GetFlipStatus), M(GetOverlayPosition), I(Surf, GetPalette),
        I(Surf, GetPixelFormat), I(Surf, GetSurfaceDesc), M(Initialize), I(Surf, IsLost), I(Surf, Lock), I(Surf, ReleaseDC),
        I(Surf, Restore), I(Surf, SetClipper), I(Surf, SetColorKey), M(SetOverlayPosition), I(Surf, SetPalette),
        I(Surf, Unlock), M(UpdateOverlay), M(UpdateOverlayDisplay), M(UpdateOverlayZOrder)};
    static const ComMethod cl[] = {
        M(QueryInterface), I(DDClip, AddRef), I(DDClip, Release), M(GetClipList), I(DDClip, GetHWnd), M(Initialize),
        M(IsClipListChanged), M(SetClipList), I(DDClip, SetHWnd)};
    static const ComMethod pl[] = {
        M(QueryInterface), I(DDPal, AddRef), I(DDPal, Release), M(GetCaps), I(DDPal, GetEntries), M(Initialize),
        I(DDPal, SetEntries)};
    vt_dd = com_vtbl("IDirectDraw", dd, sizeof dd / sizeof *dd);
    vt_surf = com_vtbl("IDirectDrawSurface", sf, sizeof sf / sizeof *sf);
    vt_clip = com_vtbl("IDirectDrawClipper", cl, sizeof cl / sizeof *cl);
    vt_pal = com_vtbl("IDirectDrawPalette", pl, sizeof pl / sizeof *pl);
}

uint32_t ddraw_create(void) {
    vtables();
    DDraw *dd = calloc(1, sizeof *dd); dd->refs = 1;
    return com_new(vt_dd, dd);
}
