/* Presentation: one SDL renderer on the game window and a streaming RGB565 texture ("the screen").
 * The game runs in 800x600x16 (565), so the common case is a straight row copy.
 *
 * Threaded mode (PS Vita): a render thread on another core owns the renderer. A full-frame present of
 * the primary surface only hands it the guest pointer; the copy, the texture upload and the vsync wait
 * all happen off the game thread. (Reading guest memory while the game keeps running can at worst tear
 * a frame; the game draws into a back buffer and blits once per frame, so in practice it does not.) */
#include "host.h"
#include <SDL.h>
#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#endif

SDL_Window *host_main_sdl_window(void);
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tw, th;
static uint16_t *screen;     /* host copy of the visible screen, RGB565 */
int video_frames;
#ifdef __vita__
#define THREADED 1
#else
#define THREADED 0
#endif
static SDL_Thread *rthread;
static SDL_mutex *pend_mx, *screen_mx;
static SDL_cond *pend_cv;
static int pending, rt_ready;
static const uint8_t *pend_src;   /* guest 565 frame to copy, or NULL = upload `screen` as is */
static int pend_pitch;
static double rt_busy;            /* render-thread seconds (separate core) */
double prof_acc[PROF_N];
double prof_now(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
static void prof_report(void) {
    static double t0; static int f0;
    double t = prof_now();
    if (t0 == 0) { t0 = t; f0 = video_frames; return; }
    if (t - t0 < 5.0) return;
    double el = t - t0; int fr = video_frames - f0;
    double other = el; for (int i = 0; i < PROF_N; i++) other -= prof_acc[i];
    if (fr < 1) fr = 1;
    hlog("PROF %.1f fps | per frame ms: blit %.2f upload %.2f present %.2f sleep %.2f wait %.2f io %.2f text %.2f game %.2f\n",
         fr / el, prof_acc[PROF_BLIT] * 1000 / fr, prof_acc[PROF_UPLOAD] * 1000 / fr, prof_acc[PROF_PRESENT] * 1000 / fr,
         prof_acc[PROF_SLEEP] * 1000 / fr, prof_acc[PROF_WAIT] * 1000 / fr, prof_acc[PROF_IO] * 1000 / fr,
         prof_acc[PROF_TEXT] * 1000 / fr, other * 1000 / fr);
    uint32_t galloc_top(void), galloc_live(void);
    hlog("PROF guest heap: %.1f MB live, top %.1f MB of %.1f MB\n", galloc_live() / 1048576.0,
         (galloc_top() - HEAP_LO) / 1048576.0, (HEAP_HI - HEAP_LO) / 1048576.0);
#ifdef __vita__
    { SceKernelFreeMemorySizeInfo fm = {sizeof fm}; sceKernelGetFreeMemorySize(&fm);
      hlog("PROF system memory free: %.1f MB user, %.1f MB cdram\n", fm.size_user / 1048576.0, fm.size_cdram / 1048576.0); }
#endif
    if (THREADED) hlog("PROF render thread: %.2f ms/frame (own core)\n", rt_busy * 1000 / fr);
    rt_busy = 0;
    for (int i = 0; i < PROF_N; i++) prof_acc[i] = 0;
    t0 = t; f0 = video_frames;
}
#if !defined(__vita__) && !defined(_WIN32)
#include <signal.h>
static volatile sig_atomic_t dump_req;
static void on_usr1(int sig) { (void)sig; dump_req = 1; }
#else
static int dump_req;
#endif
void video_dump(const char *path);
void video_present(void);

static void create_renderer(void) {
    SDL_Window *win = host_main_sdl_window();
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, tw, th);
#ifndef __ANDROID__
    SDL_RenderSetLogicalSize(ren, tw, th);
#endif
}
static void draw_and_present(void) {
#ifdef __ANDROID__
    void touchui_render(SDL_Renderer *r, SDL_Texture *t, int w, int h);   /* game + side buttons */
    touchui_render(ren, tex, tw, th);
#else
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
#endif
    SDL_RenderPresent(ren);
}
static int render_main(void *arg) {
    (void)arg;
    create_renderer();
    SDL_LockMutex(pend_mx); rt_ready = 1; SDL_CondBroadcast(pend_cv);
    for (;;) {
        while (!pending) SDL_CondWait(pend_cv, pend_mx);
        const uint8_t *src = pend_src; int pitch = pend_pitch;
        pending = 0;
        SDL_UnlockMutex(pend_mx);
        double t = prof_now();
        SDL_LockMutex(screen_mx);
        if (src) for (int y = 0; y < th; y++) memcpy(screen + (size_t)y * tw, src + (size_t)y * pitch, (size_t)tw * 2);
        SDL_UpdateTexture(tex, NULL, screen, tw * 2);
        SDL_UnlockMutex(screen_mx);
        draw_and_present();
        rt_busy += prof_now() - t;
        SDL_LockMutex(pend_mx);
    }
    return 0;
}

void video_set_mode(int w, int h) {
    SDL_Window *win = host_main_sdl_window();
    if (!win) return;
#if !defined(__vita__) && !defined(_WIN32)
    signal(SIGUSR1, on_usr1);
#endif
    if (screen && tw == w && th == h) return;
    if (THREADED) {
        if (rthread) { if (tw != w || th != h) hlog("video: mode change to %dx%d ignored (threaded)\n", w, h); return; }
        tw = w; th = h; screen = calloc((size_t)w * h, 2);
        pend_mx = SDL_CreateMutex(); screen_mx = SDL_CreateMutex(); pend_cv = SDL_CreateCond();
        rthread = SDL_CreateThread(render_main, "render", NULL);
        SDL_LockMutex(pend_mx); while (!rt_ready) SDL_CondWait(pend_cv, pend_mx); SDL_UnlockMutex(pend_mx);
        hlog("video: screen %dx%d (render thread)\n", w, h);
        return;
    }
    if (!ren) { tw = w; th = h; create_renderer(); }
    if (tex && tw == w && th == h && screen) return;
    if (tex && (tw != w || th != h)) {
        SDL_DestroyTexture(tex); tw = w; th = h;
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w, h);
#ifndef __ANDROID__
        SDL_RenderSetLogicalSize(ren, w, h);
#endif
    }
    free(screen); screen = calloc((size_t)w * h, 2);
    hlog("video: screen %dx%d\n", w, h);
}

/* Copy a rectangle of guest pixels into the screen at (dx,dy). bpp: 8 (with pal ARGB), 16 (565 or 555), 24, 32. */
void video_blit(const uint8_t *src, int pitch, int bpp, int is555, const uint32_t *pal,
                int sw, int sh, int dx, int dy) {
    if (!screen) return;
    double pt = prof_now();
    if (THREADED) SDL_LockMutex(screen_mx);
    /* clip once */
    if (dx < 0) { src += (size_t)(-dx) * (bpp / 8); sw += dx; dx = 0; }
    if (dy < 0) { src += (size_t)(-dy) * pitch; sh += dy; dy = 0; }
    if (dx + sw > tw) sw = tw - dx;
    if (dy + sh > th) sh = th - dy;
    if (sw <= 0 || sh <= 0) { if (THREADED) SDL_UnlockMutex(screen_mx); return; }
    for (int y = 0; y < sh; y++) {
        const uint8_t *s = src + (size_t)y * pitch;
        uint16_t *d = screen + (size_t)(dy + y) * tw + dx;
        if (bpp == 16 && !is555) { memcpy(d, s, (size_t)sw * 2); continue; }
        for (int x = 0; x < sw; x++) {
            uint32_t r, g, b;
            switch (bpp) {
            case 8: { uint32_t c = pal ? pal[s[x]] : s[x] * 0x010101u; r = c >> 16 & 255; g = c >> 8 & 255; b = c & 255; break; }
            case 16: { uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);       /* 555 -> 565 */
                d[x] = (uint16_t)((v & 0x7FE0) << 1 | (v & 0x200) >> 4 | (v & 31)); continue; }
            case 24: r = s[3 * x + 2]; g = s[3 * x + 1]; b = s[3 * x]; break;
            default: r = s[4 * x + 2]; g = s[4 * x + 1]; b = s[4 * x]; break;
            }
            d[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        }
    }
    if (THREADED) SDL_UnlockMutex(screen_mx);
    prof_acc[PROF_BLIT] += prof_now() - pt;
}

static void submit(const uint8_t *src, int pitch) {
    SDL_LockMutex(pend_mx);
    pend_src = src; pend_pitch = pitch; pending = 1;     /* latest frame wins if the renderer is behind */
    SDL_CondSignal(pend_cv);
    SDL_UnlockMutex(pend_mx);
    video_frames++;
    prof_report();
    gil_yield();                    /* once per frame: let the game's audio streaming thread run */
}

/* Present a whole guest surface (DirectDraw primary). Fast path: 565 frame of exactly the screen size. */
void video_present_surface(const uint8_t *src, int pitch, int bpp, int is555, const uint32_t *pal, int w, int h) {
    if (THREADED && rthread && bpp == 16 && !is555 && w == tw && h == th) { submit(src, pitch); return; }
    video_blit(src, pitch, bpp, is555, pal, w, h, 0, 0);
    video_present();
}

void video_present(void) {
    if (THREADED) { if (rthread) submit(NULL, 0); return; }
    if (!tex) return;
    double pt = prof_now();
    SDL_UpdateTexture(tex, NULL, screen, tw * 2);
#ifdef __ANDROID__
    void touchui_render(SDL_Renderer *r, SDL_Texture *t, int w, int h);   /* game + side buttons */
    touchui_render(ren, tex, tw, th);
#else
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
#endif
    double pt2 = prof_now();
    gil_release();                  /* vsync wait: let other guest threads run */
    SDL_RenderPresent(ren);
    gil_acquire();
    double pt3 = prof_now();
    prof_acc[PROF_UPLOAD] += pt2 - pt; prof_acc[PROF_PRESENT] += pt3 - pt2;
    video_frames++;
    prof_report();
    if (dump_req) {
        dump_req = 0;
        const char *p = getenv("JOEY_DUMP") ? getenv("JOEY_DUMP") : "screen.bmp";
        video_dump(p);
        uint32_t galloc_top(void);
        hlog("video: dumped frame %d to %s | guest heap top 0x%08x (%.1f MB used above heap base)\n",
             video_frames, p, galloc_top(), (galloc_top() - HEAP_LO) / 1048576.0);
    }
}

/* PNG-less debug dump: writes the current screen as a 32-bit BMP */
void video_dump(const char *path) {
    if (!screen) return;
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t sz = 54 + (uint32_t)tw * th * 4;
    uint8_t h[54] = {'B', 'M'};
    memcpy(h + 2, &sz, 4); h[10] = 54; h[14] = 40;
    int32_t w = tw, hh = -th; memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
    h[26] = 1; h[28] = 32;
    fwrite(h, 1, 54, f);
    for (size_t i = 0; i < (size_t)tw * th; i++) {
        uint16_t v = screen[i]; uint32_t r = v >> 11, g = v >> 5 & 63, b = v & 31;
        uint32_t c = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        fwrite(&c, 4, 1, f);
    }
    fclose(f);
}
