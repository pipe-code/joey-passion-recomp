/* USER32 emulation on top of SDL2: windows, classes, message queue, input, wsprintf. */
#include "host.h"
#include <SDL.h>
#include <strings.h>

uint32_t host_ticks(void);

/* ---------------------------------------------------------------- classes & windows */
typedef struct { char name[64]; uint32_t atom, style, wndproc, cbWndExtra, hIcon, hCursor, hbr; } WClass;
typedef struct {
    uint32_t hwnd, wndproc, style, exstyle, parent, menu, id, userdata;
    char cls[64], title[256];
    int x, y, w, h;          /* outer rect in fake screen coordinates */
    int cw, ch;              /* client size */
    int visible;
    uint8_t extra[64];
    SDL_Window *sdl;
} Wnd;

#define MAXCLS 32
static WClass classes[MAXCLS];
static int nclasses;
static Wnd *windows[64];
static int nwindows;
Wnd *main_wnd;
static uint32_t focus_hwnd;

Wnd *wnd_get(uint32_t h) { return handle_get(h, H_WINDOW); }

/* window frame metrics (Windows XP classic) used consistently by AdjustWindowRect & CreateWindowEx */
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_CAPTION 0x00C00000u
#define WS_THICKFRAME 0x00040000u
#define WS_CHILD 0x40000000u
static void frame_of(uint32_t style, int menu, int *l, int *t, int *r, int *b) {
    int f = (style & WS_THICKFRAME) ? 4 : (style & (WS_BORDER | WS_DLGFRAME)) ? ((style & WS_DLGFRAME) ? 3 : 1) : 0;
    *l = *r = *b = f; *t = f;
    if ((style & WS_CAPTION) == WS_CAPTION) *t += 19;
    if (menu) *t += 19;
}

static const char *class_name(uint32_t p, char *buf) {
    if (p < 0x10000) { for (int i = 0; i < nclasses; i++) if (classes[i].atom == p) return classes[i].name; snprintf(buf, 16, "#%u", p); return buf; }
    return GSTR(p);
}

API(RegisterClassExA, 1) {
    uint32_t w = A(0);
    if (nclasses == MAXCLS) return 0;
    WClass *c = &classes[nclasses];
    snprintf(c->name, sizeof c->name, "%s", GSTR(RD32(w + 40)));
    c->style = RD32(w + 4); c->wndproc = RD32(w + 8); c->cbWndExtra = RD32(w + 16);
    c->hIcon = RD32(w + 24); c->hCursor = RD32(w + 28); c->hbr = RD32(w + 32);
    c->atom = 0xC000 + nclasses++;
    hlog("RegisterClassExA(%s, wndproc 0x%x)\n", c->name, c->wndproc);
    return c->atom;
}

API(AdjustWindowRect, 3) {
    int l, t, r, b; frame_of(A(1), A(2), &l, &t, &r, &b);
    uint32_t p = A(0);
    WR32(p, RD32(p) - l); WR32(p + 4, RD32(p + 4) - t); WR32(p + 8, RD32(p + 8) + r); WR32(p + 12, RD32(p + 12) + b);
    return 1;
}

API(GetSystemMetrics, 1) {
    static const int m[] = {[0] = 1024, [1] = 768, [4] = 19, [5] = 1, [6] = 1, [7] = 3, [8] = 3, [13] = 32, [14] = 32,
                            [15] = 19, [16] = 1024, [17] = 768 - 19, [32] = 4, [33] = 4, [43] = 3};
    uint32_t i = A(0);
    return i < sizeof m / sizeof *m ? (uint32_t)m[i] : 0;
}

static int sdl_ready;
static void sdl_init(void) {
    if (sdl_ready) return;
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");      /* back is Esc (game menu), not "close the app" */
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");            /* fingers go to touchui.c (game area + side buttons) */
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) { hlog("SDL_Init: %s\n", SDL_GetError()); exit(1); }
    sdl_ready = 1;
}

static uint32_t send_message_ctx(CPU *ctx, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd);
    if (!w || !w->wndproc) return 0;
    return guest_call(ctx, w->wndproc, 4, hwnd, msg, wp, lp);
}
#define send_message(h, m, w, l) send_message_ctx(ctx, h, m, w, l)
uint32_t send_message_main(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    return send_message_ctx(&main_cpu, hwnd, msg, wp, lp);
}

API(CreateWindowExA, 12) {      /* exstyle, class, title, style, x, y, w, h, parent, menu, inst, param */
    char cbuf[16];
    const char *cn = class_name(A(1), cbuf);
    WClass *c = NULL;
    for (int i = 0; i < nclasses; i++) if (strcasecmp(classes[i].name, cn) == 0) c = &classes[i];
    if (!c) { hlog("CreateWindowExA: unknown class %s\n", cn); last_error = 1407; return 0; }
    Wnd *w = calloc(1, sizeof *w);
    w->wndproc = c->wndproc; w->style = A(3); w->exstyle = A(0); w->parent = A(8); w->menu = A(9);
    snprintf(w->cls, sizeof w->cls, "%s", c->name);
    snprintf(w->title, sizeof w->title, "%s", A(2) ? GSTR(A(2)) : "");
    int32_t x = (int32_t)A(4), y = (int32_t)A(5), ww = (int32_t)A(6), hh = (int32_t)A(7);
    if ((uint32_t)x == 0x80000000u) x = 0; if ((uint32_t)y == 0x80000000u) y = 0;
    if ((uint32_t)ww == 0x80000000u) ww = 640; if ((uint32_t)hh == 0x80000000u) hh = 480;
    int l, t, r, b; frame_of(w->style, 0, &l, &t, &r, &b);
    w->x = x; w->y = y; w->w = ww; w->h = hh; w->cw = ww - l - r; w->ch = hh - t - b;
    w->hwnd = handle_new(H_WINDOW, w);
    windows[nwindows++] = w;
    hlog("CreateWindowExA(class %s, title \"%s\", style 0x%x, ex 0x%x, %d,%d %dx%d -> client %dx%d) = 0x%x\n",
         cn, w->title, w->style, w->exstyle, x, y, ww, hh, w->cw, w->ch, w->hwnd);
    if (!(w->style & WS_CHILD)) {
        sdl_init();
#ifdef __vita__
        /* the Vita has one 960x544 framebuffer: the renderer letterboxes the 800x600 game into it */
        w->sdl = SDL_CreateWindow("Joey", 0, 0, 960, 544, SDL_WINDOW_HIDDEN);
#elif defined(__ANDROID__)
        /* immersive full screen (no status / navigation bars); the renderer letterboxes the 4:3 game */
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        w->sdl = SDL_CreateWindow("Joey", 0, 0, 800, 600, SDL_WINDOW_HIDDEN | SDL_WINDOW_FULLSCREEN);
#else
        w->sdl = SDL_CreateWindow(w->title[0] ? w->title : "Joey", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  w->cw > 0 ? w->cw : 800, w->ch > 0 ? w->ch : 600, SDL_WINDOW_HIDDEN);
#endif
        if (!main_wnd) main_wnd = w;
    }
    /* CREATESTRUCTA on the guest heap */
    uint32_t cs = galloc(48, 1);
    WR32(cs + 0, A(11)); WR32(cs + 4, A(10)); WR32(cs + 8, A(9)); WR32(cs + 12, A(8));
    WR32(cs + 16, hh); WR32(cs + 20, ww); WR32(cs + 24, y); WR32(cs + 28, x);
    WR32(cs + 32, w->style); WR32(cs + 36, A(2)); WR32(cs + 40, A(1)); WR32(cs + 44, w->exstyle);
    uint32_t hwnd = w->hwnd;
    if (!send_message(hwnd, 0x81 /* WM_NCCREATE */, 0, cs)) hlog("  WM_NCCREATE returned 0 (ignored)\n");
    int32_t rc = (int32_t)send_message(hwnd, 0x01 /* WM_CREATE */, 0, cs);
    gfree(cs);
    if (rc == -1) { hlog("  WM_CREATE failed\n"); return 0; }
    if (w->style & 0x10000000u /* WS_VISIBLE */) { w->visible = 1; if (w->sdl) SDL_ShowWindow(w->sdl); }
    return hwnd;
}

API(ShowWindow, 2) {
    Wnd *w = wnd_get(A(0)); if (!w) return 0;
    int was = w->visible;
    w->visible = A(1) != 0;
    hlog("ShowWindow(0x%x, %d)\n", A(0), A(1));
    if (w->sdl) { if (w->visible) SDL_ShowWindow(w->sdl); else SDL_HideWindow(w->sdl); }
    if (w->visible && !was) {
        send_message(A(0), 0x18 /* WM_SHOWWINDOW */, 1, 0);
        send_message(A(0), 0x05 /* WM_SIZE */, 0, (uint32_t)(w->ch << 16 | (w->cw & 0xFFFF)));
    }
    return (uint32_t)was;
}

API(DestroyWindow, 1) {
    Wnd *w = wnd_get(A(0)); if (!w) return 0;
    send_message(A(0), 0x02 /* WM_DESTROY */, 0, 0);
    send_message(A(0), 0x82 /* WM_NCDESTROY */, 0, 0);
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    for (int i = 0; i < nwindows; i++) if (windows[i] == w) windows[i] = windows[--nwindows];
    if (main_wnd == w) main_wnd = NULL;
    handle_close(A(0)); free(w);
    return 1;
}

API(DefWindowProcA, 4) {
    switch (A(1)) {
    case 0x10: {                    /* WM_CLOSE: default action destroys the window */
        uint32_t saved_esp = ctx->esp;
        guest_call(ctx, thunk_for("user32.dll", "DestroyWindow"), 1, A(0));
        ctx->esp = saved_esp;
        return 0; }
    case 0x81: return 1;            /* WM_NCCREATE */
    case 0x14: return 1;            /* WM_ERASEBKGND */
    default: return 0;
    }
}

API(CallWindowProcA, 5) { return guest_call(ctx, A(0), 4, A(1), A(2), A(3), A(4)); }

static uint32_t *long_slot(Wnd *w, int32_t i) {
    switch (i) {
    case -4: return &w->wndproc; case -16: return &w->style; case -20: return &w->exstyle;
    case -21: return &w->userdata; case -12: return &w->id;
    }
    if (i >= 0 && i + 4 <= (int32_t)sizeof w->extra) return (uint32_t *)(w->extra + i);
    return NULL;
}
API(GetWindowLongA, 2) {
    Wnd *w = wnd_get(A(0)); if (!w) return 0;
    if ((int32_t)A(1) == -6) return IMAGE_LO;
    uint32_t *s = long_slot(w, (int32_t)A(1));
    return s ? *s : 0;
}
API(SetWindowLongA, 3) {
    Wnd *w = wnd_get(A(0)); if (!w) return 0;
    uint32_t *s = long_slot(w, (int32_t)A(1)); if (!s) return 0;
    uint32_t old = *s; *s = A(2);
    TRACE_API("SetWindowLongA(0x%x, %d, 0x%x) old 0x%x\n", A(0), (int32_t)A(1), A(2), old);
    return old;
}

API(SetWindowPos, 7) {          /* hwnd, after, x, y, cx, cy, flags */
    Wnd *w = wnd_get(A(0)); if (!w) return 0;
    uint32_t fl = A(6);
    if (!(fl & 2)) { w->x = (int32_t)A(2); w->y = (int32_t)A(3); }
    if (!(fl & 1)) {
        int l, t, r, b; frame_of(w->style, 0, &l, &t, &r, &b);
        w->w = (int32_t)A(4); w->h = (int32_t)A(5); w->cw = w->w - l - r; w->ch = w->h - t - b;
        if (w->sdl && w->cw > 0 && w->ch > 0) SDL_SetWindowSize(w->sdl, w->cw, w->ch);
    }
    if (fl & 0x40) { w->visible = 1; if (w->sdl) SDL_ShowWindow(w->sdl); }      /* SWP_SHOWWINDOW */
    hlog("SetWindowPos(0x%x, %d,%d %dx%d flags 0x%x) client %dx%d\n", A(0), w->x, w->y, w->w, w->h, fl, w->cw, w->ch);
    return 1;
}
static void put_rect(uint32_t p, int l, int t, int r, int b) { WR32(p, l); WR32(p + 4, t); WR32(p + 8, r); WR32(p + 12, b); }
API(GetWindowRect, 2) { Wnd *w = wnd_get(A(0)); if (!w) return 0; put_rect(A(1), w->x, w->y, w->x + w->w, w->y + w->h); return 1; }
API(GetClientRect, 2) { Wnd *w = wnd_get(A(0)); if (!w) return 0; put_rect(A(1), 0, 0, w->cw, w->ch); return 1; }
/* Exclusive fullscreen (DirectDraw DDSCL_FULLSCREEN): the game window covers the screen, so client
 * coordinates are screen coordinates (otherwise GetCursorPos would be offset by the old window position). */
static int fullscreen;
void host_set_fullscreen(int on) { fullscreen = on; }
static void client_origin(Wnd *w, int *x, int *y) {
    if (fullscreen && w == main_wnd) { *x = *y = 0; return; }
    int l, t, r, b; frame_of(w->style, 0, &l, &t, &r, &b); *x = w->x + l; *y = w->y + t;
}
API(ClientToScreen, 2) { Wnd *w = wnd_get(A(0)); if (!w) return 0; int x, y; client_origin(w, &x, &y); WR32(A(1), RD32(A(1)) + x); WR32(A(1) + 4, RD32(A(1) + 4) + y); return 1; }
API(ScreenToClient, 2) { Wnd *w = wnd_get(A(0)); if (!w) return 0; int x, y; client_origin(w, &x, &y); WR32(A(1), RD32(A(1)) - x); WR32(A(1) + 4, RD32(A(1) + 4) - y); return 1; }
API(SetRect, 5) { put_rect(A(0), A(1), A(2), A(3), A(4)); return 1; }
API(OffsetRect, 3) { uint32_t p = A(0); int32_t dx = A(1), dy = A(2); put_rect(p, RD32(p) + dx, RD32(p + 4) + dy, RD32(p + 8) + dx, RD32(p + 12) + dy); return 1; }
API(IntersectRect, 3) {
    int32_t l = (int32_t)RD32(A(1)), t = RD32(A(1) + 4), r = RD32(A(1) + 8), b = RD32(A(1) + 12);
    int32_t l2 = RD32(A(2)), t2 = RD32(A(2) + 4), r2 = RD32(A(2) + 8), b2 = RD32(A(2) + 12);
    l = l > l2 ? l : l2; t = t > t2 ? t : t2; r = r < r2 ? r : r2; b = b < b2 ? b : b2;
    if (l >= r || t >= b) { put_rect(A(0), 0, 0, 0, 0); return 0; }
    put_rect(A(0), l, t, r, b); return 1;
}

API(GetActiveWindow, 0) { return main_wnd ? main_wnd->hwnd : 0; }
API(SetForegroundWindow, 1) { return 1; }
API(SetFocus, 1) { uint32_t o = focus_hwnd; focus_hwnd = A(0); return o; }
API(GetWindow, 2) { return 0; }
API(GetTopWindow, 1) { return (!A(0) && main_wnd) ? main_wnd->hwnd : 0; }
API(GetWindowThreadProcessId, 2) { if (A(1)) WR32(A(1), 1); return 1; }
API(EnumWindows, 2) {
    for (int i = 0; i < nwindows; i++)
        if (!(windows[i]->style & WS_CHILD) && !guest_call(ctx, A(0), 2, windows[i]->hwnd, A(1))) break;
    return 1;
}
API(LoadIconA, 2) { return 0x00F00000u | (A(1) & 0xFFFF); }
API(LoadCursorA, 2) { return 0x00F10000u | (A(1) & 0xFFFF); }
static int cursor_count = 0;
API(ShowCursor, 1) { cursor_count += A(0) ? 1 : -1; if (sdl_ready) SDL_ShowCursor(cursor_count >= 0); return (uint32_t)cursor_count; }
static int mouse_x, mouse_y;
API(SetCursorPos, 2) {
#ifdef __ANDROID__
    /* no pointer to warp on a touch screen (and window pixels are not game pixels): move the game's cursor */
    if (main_wnd) { int x, y; client_origin(main_wnd, &x, &y); mouse_x = (int32_t)A(0) - x; mouse_y = (int32_t)A(1) - y; }
#else
    if (main_wnd && main_wnd->sdl) { int x, y; client_origin(main_wnd, &x, &y); SDL_WarpMouseInWindow(main_wnd->sdl, (int32_t)A(0) - x, (int32_t)A(1) - y); }
#endif
    return 1;
}
API(GetCursorPos, 1) {
    int x = 0, y = 0; if (main_wnd) client_origin(main_wnd, &x, &y);
    WR32(A(0), mouse_x + x); WR32(A(0) + 4, mouse_y + y); return 1;
}
API(MessageBoxA, 4) {
    hlog("MessageBoxA(\"%s\", \"%s\", type 0x%x)\n", A(2) ? GSTR(A(2)) : "", A(1) ? GSTR(A(1)) : "", A(3));
    switch (A(3) & 0xF) { case 3: case 4: return 6; default: return 1; }     /* IDYES / IDOK */
}

/* menus: the game window has none */
API(GetMenu, 1) { return 0; }
API(SetMenu, 2) { return 1; }
API(DrawMenuBar, 1) { return 1; }
API(CheckMenuItem, 3) { return 0xFFFFFFFFu; }
API(GetMenuState, 3) { return 0xFFFFFFFFu; }

/* ---------------------------------------------------------------- message queue */
typedef struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } Msg;
#define QN 512
static Msg queue[QN];
static int qhead, qtail;
static uint8_t keystate[256];

static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int n = (qtail + 1) % QN;
    if (n == qhead) return;          /* queue full: drop */
    queue[qtail] = (Msg){hwnd, msg, wp, lp, host_ticks(), mouse_x, mouse_y};
    qtail = n;
}

static uint32_t vk_of(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return (uint32_t)k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70u + (uint32_t)(k - SDLK_F1);
    switch (k) {
    case SDLK_RETURN: return 0x0D; case SDLK_ESCAPE: case SDLK_AC_BACK: return 0x1B; case SDLK_SPACE: return 0x20; case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09; case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28; case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12; case SDLK_DELETE: return 0x2E; case SDLK_INSERT: return 0x2D;
    case SDLK_HOME: return 0x24; case SDLK_END: return 0x23; case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22;
    }
    return 0;
}

/* gamepad (Vita buttons, or any controller on desktop) drives a virtual mouse */
static SDL_GameController *pad;
static int pad_l, pad_r, pad_start;
/* Touch screen taps. On a PC the cursor always rests on a button before it is clicked, and a click lasts longer
 * than a frame. A tap jumps the cursor and presses and releases at once, so the game (which updates hover and
 * polls the button once per frame) misses it. For touch: move the cursor first, press TOUCH_HOVER_MS later and
 * keep the button down for at least TOUCH_CLICK_MS. */
#define TOUCH_HOVER_MS 60
#define TOUCH_CLICK_MS 100
static int tdown_pending, tup_pending, theld, touch_mode;
static uint32_t tdown_due, tup_due, tdown_at;
static float pad_fx = -1, pad_fy;
static uint32_t pad_last_ms;
/* Mouse wheel. The game keeps a wheel counter (WM_MOUSEWHEEL handler at 0x47fbf0) that its scroll lists and
 * long descriptions read. notches > 0 = up / back. */
static uint32_t mk_state(void);
static void post_wheel(uint32_t h, int notches) {
    post(h, 0x20A, (uint32_t)(int16_t)(notches * 120) << 16 | mk_state(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
}
static uint32_t rstick_next;
/* Right stick: up/left scroll back, down/right scroll forward; repeat rate grows with the tilt. */
static __attribute__((unused)) void rstick_pump(uint32_t h) {
    if (!pad) return;
    float rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.f;
    float ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.f;
    float m = fabsf(rx) > fabsf(ry) ? rx : ry;
    if (fabsf(m) < 0.35f) { rstick_next = 0; return; }
    uint32_t now = SDL_GetTicks();
    if (rstick_next && now < rstick_next) return;
    post_wheel(h, m < 0 ? 1 : -1);
    float t = (fabsf(m) - 0.35f) / 0.65f; if (t > 1) t = 1;
    rstick_next = now + (uint32_t)(260 - 190 * t);       /* 260 ms at the threshold .. 70 ms at full tilt */
}
int host_pad_start(void) { return pad_start; }      /* START -> Escape in DirectInput too */

static uint32_t mk_state(void) {
    uint32_t s = SDL_GetMouseState(NULL, NULL), r = 0;
    if (touch_mode) s &= ~SDL_BUTTON_LMASK;
    if (pad_l || theld) r |= 1; if (pad_r) r |= 2;
    if (s & SDL_BUTTON_LMASK) r |= 1; if (s & SDL_BUTTON_RMASK) r |= 2; if (s & SDL_BUTTON_MMASK) r |= 0x10;
    if (keystate[0x10] & 0x80) r |= 4; if (keystate[0x11] & 0x80) r |= 8;
    return r;
}

/* Debug: scripted clicks for reproducing bugs without playing, JOEY_AUTOCLICK="ms:x,y;ms:x,y;..."
 * (ms since start, game coordinates). */
static void autoclick(uint32_t h) {
    static const char *p; static int init, held; static uint32_t t0, at, release; static int ax, ay, have;
    if (!init) { init = 1; p = getenv("JOEY_AUTOCLICK"); t0 = SDL_GetTicks(); }
    if (held) {                     /* keep the button down for a few frames: the game polls it */
        mouse_x = ax; mouse_y = ay;
        if (SDL_GetTicks() < release) return;
        held = 0; pad_l = 0;
        post(h, 0x202, 0, (uint32_t)(ay << 16 | (ax & 0xFFFF)));
    }
    for (;;) {
        if (!have) {
            if (!p || !*p) return;
            if (sscanf(p, "%u:%d,%d", &at, &ax, &ay) != 3) { p = NULL; return; }
            p = strchr(p, ';'); if (p) p++;
            have = 1;
        }
        if (SDL_GetTicks() - t0 < at) return;
        mouse_x = ax; mouse_y = ay;
        uint32_t xy = (uint32_t)(ay << 16 | (ax & 0xFFFF));
        if (have == 1) { post(h, 0x200, 0, xy); have = 2; at += 300; return; }    /* hover first */
        post(h, 0x201, 1, xy);
        pad_l = 1; held = 1; release = SDL_GetTicks() + 150;
        hlog("autoclick %d,%d at %u ms\n", ax, ay, at);
        have = 0;
        return;
    }
}

static void touch_pump(uint32_t h) {
    uint32_t now = SDL_GetTicks(), lp = (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF));
    if (tdown_pending && (int32_t)(now - tdown_due) >= 0) {
        tdown_pending = 0; theld = 1; tdown_at = now;
        post(h, 0x201, mk_state(), lp);
    }
    if (tup_pending && !tdown_pending && (int32_t)(now - tup_due) >= 0) {
        tup_pending = 0; theld = 0;
        post(h, 0x202, mk_state(), lp);
    }
}
static void touch_button(uint32_t h, int down) {
    uint32_t now = SDL_GetTicks(), lp = (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF));
    if (down) {
        if (tdown_pending || tup_pending) { tdown_due = tup_due = now; touch_pump(h); }    /* finish the previous tap */
        post(h, 0x200, mk_state(), lp);                                                   /* hover first */
        tdown_pending = 1; tdown_due = now + TOUCH_HOVER_MS;
    } else {
        uint32_t pressed = tdown_pending ? tdown_due : tdown_at;
        tup_pending = 1; tup_due = pressed + TOUCH_CLICK_MS;
        if ((int32_t)(now - tup_due) > 0) tup_due = now;
        touch_pump(h);
    }
}

/* ---- the Android touch layer (touchui.c) drives the game through these; coordinates are game pixels */
static int rclick_pending, esc_pending; static uint32_t rclick_due, esc_due;
static void hold_pump(uint32_t h) {
    uint32_t now = SDL_GetTicks();
    if (rclick_pending && (int32_t)(now - rclick_due) >= 0) {
        rclick_pending = 0; pad_r = 0;
        post(h, 0x205, mk_state(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
    }
    if (esc_pending && (int32_t)(now - esc_due) >= 0) {
        esc_pending = 0; pad_start = 0; keystate[0x1B] = 0;
        post(h, 0x101, 0x1B, 0xC0000001u);
    }
}
void host_touch_hover(int x, int y) {
    if (!main_wnd) return;
    mouse_x = x; mouse_y = y; touch_mode = 1;
    post(main_wnd->hwnd, 0x200, mk_state(), (uint32_t)(y << 16 | (x & 0xFFFF)));
}
void host_touch_click(int x, int y) {
    if (!main_wnd) return;
    mouse_x = x; mouse_y = y; touch_mode = 1;
    touch_button(main_wnd->hwnd, 1); touch_button(main_wnd->hwnd, 0);
}
void host_right_click(void) {           /* at the cursor, held a few frames like a tap */
    if (!main_wnd || rclick_pending) return;
    pad_r = 1; rclick_pending = 1; rclick_due = SDL_GetTicks() + TOUCH_CLICK_MS;
    post(main_wnd->hwnd, 0x204, mk_state(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
}
void host_escape(void) {                /* Esc for both the message queue and DirectInput */
    if (!main_wnd || esc_pending) return;
    pad_start = 1; keystate[0x1B] = 0x80; esc_pending = 1; esc_due = SDL_GetTicks() + 120;
    post(main_wnd->hwnd, 0x100, 0x1B, 1);
}
void host_wheel(int notches) { if (main_wnd) post_wheel(main_wnd->hwnd, notches); }
int host_mouse_x(void) { return mouse_x; }
int host_mouse_y(void) { return mouse_y; }

void host_pump_events(void) {
    if (!sdl_ready || !main_wnd) return;
    uint32_t h = main_wnd->hwnd;
    SDL_Event e;
    autoclick(h);
    void host_text_poll(void); int host_text_ime_open(void);
    host_text_poll();
    touch_pump(h);
    hold_pump(h);
#if defined(__vita__) || defined(__ANDROID__)
    rstick_pump(h);
#endif
#ifdef __ANDROID__
    void touchui_pump(void); touchui_pump();
#endif
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: post(h, 0x10, 0, 0); break;                                   /* WM_CLOSE */
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) post(h, 0x10, 0, 0);
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) { post(h, 0x1C, 1, 0); post(h, 0x06, 1, 0); }
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { post(h, 0x1C, 0, 0); post(h, 0x06, 0, 0); }
            break;
#ifdef __ANDROID__
        case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION: {
            int touchui_event(const SDL_Event *ev);
            if (!host_text_ime_open()) touchui_event(&e);
            break; }
#endif
        case SDL_MOUSEMOTION:
            if (getenv("JOEY_AUTOCLICK") || host_text_ime_open()) break;     /* scripted input: ignore the real mouse */
#ifdef __ANDROID__
            if (e.motion.which == SDL_TOUCH_MOUSEID) break;                  /* fingers: touchui.c */
#endif
            mouse_x = e.motion.x; mouse_y = e.motion.y;
#ifdef __ANDROID__
            { void touchui_to_game(int *x, int *y); touchui_to_game(&mouse_x, &mouse_y); }     /* a real mouse (DeX, Chromebook) */
#endif
            post(h, 0x200, mk_state(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
            break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            if (host_text_ime_open()) break;            /* touches belong to the on-screen keyboard */
#ifdef __ANDROID__
            if (e.button.which == SDL_TOUCH_MOUSEID) break;
#endif
            int down = e.type == SDL_MOUSEBUTTONDOWN;
            mouse_x = e.button.x; mouse_y = e.button.y;
#ifdef __ANDROID__
            { void touchui_to_game(int *x, int *y); touchui_to_game(&mouse_x, &mouse_y); }
#endif
            touch_mode = e.button.which == SDL_TOUCH_MOUSEID;
            if (touch_mode && e.button.button == SDL_BUTTON_LEFT) { touch_button(h, down); break; }
            uint32_t m = e.button.button == SDL_BUTTON_LEFT ? 0x201 : e.button.button == SDL_BUTTON_RIGHT ? 0x204 : 0x207;
            post(h, m + (down ? 0 : 1), mk_state(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
            break; }
        case SDL_MOUSEWHEEL:
            if (e.wheel.y) post_wheel(h, e.wheel.y > 0 ? 1 : -1);
            break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            uint32_t vk = vk_of(e.key.keysym.sym);
            if (!vk) break;
            int down = e.type == SDL_KEYDOWN;
            keystate[vk] = down ? 0x80 : 0;
            uint32_t lp = 1u | ((uint32_t)e.key.keysym.scancode << 16) | (down ? 0 : 0xC0000000u) | (e.key.repeat ? 0x40000000u : 0);
            post(h, down ? 0x100 : 0x101, vk, lp);
            break; }
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad) { pad = SDL_GameControllerOpen(e.cdevice.which); hlog("gamepad: %s\n", pad ? SDL_GameControllerName(pad) : "?"); }
            break;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            if (host_text_ime_open()) break;
            int down = e.type == SDL_CONTROLLERBUTTONDOWN;
            uint32_t lp = (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF));
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A: pad_l = down; post(h, down ? 0x201 : 0x202, mk_state(), lp); break;   /* Cross  */
            case SDL_CONTROLLER_BUTTON_B: pad_r = down; post(h, down ? 0x204 : 0x205, mk_state(), lp); break;   /* Circle */
#if defined(__vita__) || defined(__ANDROID__)
            case SDL_CONTROLLER_BUTTON_Y: if (down) { void host_text_reopen(void); host_text_reopen(); } break;   /* Triangle: keyboard again */
#endif
            case SDL_CONTROLLER_BUTTON_START:
                pad_start = down; keystate[0x1B] = down ? 0x80 : 0;
                post(h, down ? 0x100 : 0x101, 0x1B, down ? 1u : 0xC0000001u);
                break;
            }
            break; }
        }
    }
    /* analog stick / d-pad -> cursor motion */
    if (pad && !host_text_ime_open()) {
        uint32_t now = host_ticks(), dt = pad_last_ms ? now - pad_last_ms : 0;
        pad_last_ms = now;
        if (dt > 100) dt = 100;
        float ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32768.f;
        float ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32768.f;
        if (ax * ax + ay * ay < 0.04f) ax = ay = 0;                        /* dead zone */
        ax *= fabsf(ax); ay *= fabsf(ay);                                  /* finer control near center */
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) ax = -0.25f;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) ax = 0.25f;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) ay = -0.25f;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) ay = 0.25f;
        if (pad_fx < 0) { pad_fx = (float)mouse_x; pad_fy = (float)mouse_y; }
        if (ax || ay) {
            const float speed = 0.9f;                                      /* pixels per ms at full tilt */
            pad_fx += ax * speed * dt; pad_fy += ay * speed * dt;
            if (pad_fx < 0) pad_fx = 0; if (pad_fx > 799) pad_fx = 799;
            if (pad_fy < 0) pad_fy = 0; if (pad_fy > 599) pad_fy = 599;
            if ((int)pad_fx != mouse_x || (int)pad_fy != mouse_y) {
                mouse_x = (int)pad_fx; mouse_y = (int)pad_fy;
                post(h, 0x200, mk_state(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
            }
        } else { pad_fx = (float)mouse_x; pad_fy = (float)mouse_y; }       /* follow touch / real mouse */
    }
}

static int match(Msg *m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && m->hwnd != hwnd) return 0;
    if (lo || hi) return m->msg >= lo && m->msg <= hi;
    return 1;
}
static void write_msg(uint32_t p, Msg *m) {
    WR32(p, m->hwnd); WR32(p + 4, m->msg); WR32(p + 8, m->wp); WR32(p + 12, m->lp); WR32(p + 16, m->time);
    WR32(p + 20, m->x); WR32(p + 24, m->y);
}
static int take_msg(uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove) {
    for (int i = qhead; i != qtail; i = (i + 1) % QN) {
        if (!match(&queue[i], hwnd, lo, hi)) continue;
        Msg m = queue[i];
        if (remove) {           /* remove entry i, keeping order */
            for (int j = i; j != qhead; ) { int pj = (j - 1 + QN) % QN; queue[j] = queue[pj]; j = pj; }
            qhead = (qhead + 1) % QN;
        }
        write_msg(out, &m);
        return 1;
    }
    return 0;
}
void dplay_pump(CPU *ctx);
API(PeekMessageA, 5) { host_pump_events(); dplay_pump(ctx); gil_yield(); return (uint32_t)take_msg(A(0), A(1), A(2), A(3), A(4) & 1); }
API(GetMessageA, 4) {
    for (;;) {
        host_pump_events(); dplay_pump(ctx);
        if (take_msg(A(0), A(1), A(2), A(3), 1)) return RD32(A(0) + 4) != 0x12;     /* WM_QUIT -> 0 */
        gil_release(); SDL_WaitEventTimeout(NULL, 10); gil_acquire();
    }
}
API(PostMessageA, 4) { post(A(0), A(1), A(2), A(3)); return 1; }
API(PostQuitMessage, 1) { post(0, 0x12, A(0), 0); return 0; }
API(SendMessageA, 4) { return send_message(A(0), A(1), A(2), A(3)); }
API(TranslateMessage, 1) {
    uint32_t m = RD32(A(0) + 4), vk = RD32(A(0) + 8);
    if (m == 0x100) {
        char c = 0;
        if (vk >= 'A' && vk <= 'Z') c = (char)((keystate[0x10] & 0x80) ? vk : vk + 32);
        else if ((vk >= '0' && vk <= '9') || vk == 0x20 || vk == 0x0D || vk == 0x08 || vk == 0x1B) c = (char)vk;
        if (c) post(RD32(A(0)), 0x102 /* WM_CHAR */, (uint8_t)c, RD32(A(0) + 12));
    }
    return 1;
}
API(DispatchMessageA, 1) {
    uint32_t p = A(0), hwnd = RD32(p);
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    return guest_call(ctx, w->wndproc, 4, hwnd, RD32(p + 4), RD32(p + 8), RD32(p + 12));
}

/* ---------------------------------------------------------------- keyboard */
API(GetKeyboardState, 1) { memcpy(GPTR(A(0)), keystate, 256); return 1; }
API(GetKeyboardLayout, 1) { return 0x04090409u; }
API(MapVirtualKeyA, 2) { return A(1) == 2 ? ((A(0) >= 'A' && A(0) <= 'Z') || (A(0) >= '0' && A(0) <= '9') ? A(0) : 0) : A(0); }
API(ToAscii, 5) {
    uint32_t vk = A(0);
    if (vk == 0xBD) { WR16(A(3), '-'); return 1; }                  /* VK_OEM_MINUS */
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == ' ') {
        int shift = RD8(A(2) + 0x10) & 0x80;
        WR16(A(3), (uint16_t)((vk >= 'A' && vk <= 'Z' && !shift) ? vk + 32 : vk));
        return 1;
    }
    return 0;
}
API(WINNLSEnableIME, 2) { hlog("WINNLSEnableIME(0x%x, %u) from 0x%x\n", A(0), A(1), RD32(ctx->esp)); return 1; }

/* ---------------------------------------------------------------- wsprintf */
static int gformat(char *out, int outsz, const char *fmt, uint32_t ap) {
    int n = 0;
    #define PUT(c) do { if (n < outsz - 1) out[n] = (c); n++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        char spec[32]; int sl = 0; spec[sl++] = '%'; p++;
        while (*p && strchr("-+ #0", *p) && sl < 20) spec[sl++] = *p++;
        while (*p && ((*p >= '0' && *p <= '9') || *p == '.') && sl < 28) spec[sl++] = *p++;
        int wide = 0;
        while (*p == 'l' || *p == 'h') { if (*p == 'l') wide = 1; p++; }
        char conv = *p, tmp[512];
        if (!conv) break;
        if (conv == '%') { PUT('%'); continue; }
        uint32_t v = RD32(ap); ap += 4;
        spec[sl++] = (conv == 'S' || conv == 's') ? 's' : (conv == 'C' || conv == 'c') ? 'c' : conv; spec[sl] = 0;
        (void)wide;
        if (conv == 's' || conv == 'S') snprintf(tmp, sizeof tmp, spec, v ? GSTR(v) : "(null)");
        else if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, (int)v);
        else snprintf(tmp, sizeof tmp, spec, v);
        for (char *q = tmp; *q; q++) PUT(*q);
    }
    if (outsz) out[n < outsz ? n : outsz - 1] = 0;
    return n;
    #undef PUT
}
API(wvsprintfA, 3) { char buf[1025]; int n = gformat(buf, sizeof buf, GSTR(A(1)), A(2)); strcpy(GPTR(A(0)), buf); return (uint32_t)n; }
API_CDECL(wsprintfA) { char buf[1025]; int n = gformat(buf, sizeof buf, GSTR(A(1)), ctx->esp + 12); strcpy(GPTR(A(0)), buf); return (uint32_t)n; }

/* GDI entry points that need the window table live in gdi32.c */
const HostExport user32_exports[] = {
#define X(n) {#n, w_##n},
    X(RegisterClassExA) X(AdjustWindowRect) X(GetSystemMetrics) X(CreateWindowExA) X(ShowWindow) X(DestroyWindow)
    X(DefWindowProcA) X(CallWindowProcA) X(GetWindowLongA) X(SetWindowLongA) X(SetWindowPos) X(GetWindowRect)
    X(GetClientRect) X(ClientToScreen) X(ScreenToClient) X(SetRect) X(OffsetRect) X(IntersectRect)
    X(GetActiveWindow) X(SetForegroundWindow) X(SetFocus) X(GetWindow) X(GetTopWindow) X(GetWindowThreadProcessId)
    X(EnumWindows) X(LoadIconA) X(LoadCursorA) X(ShowCursor) X(SetCursorPos) X(GetCursorPos) X(MessageBoxA)
    X(GetMenu) X(SetMenu) X(DrawMenuBar) X(CheckMenuItem) X(GetMenuState)
    X(PeekMessageA) X(GetMessageA) X(PostMessageA) X(PostQuitMessage) X(SendMessageA) X(TranslateMessage)
    X(DispatchMessageA) X(GetKeyboardState) X(GetKeyboardLayout) X(MapVirtualKeyA) X(ToAscii) X(WINNLSEnableIME)
    X(wvsprintfA) X(wsprintfA)
#undef X
    {NULL, NULL}};

SDL_Window *host_main_sdl_window(void) { return main_wnd ? main_wnd->sdl : NULL; }
/* fake-screen position of the main window's client origin (for windowed DirectDraw primary blits) */
void host_client_origin(int *x, int *y) { *x = *y = 0; if (main_wnd) client_origin(main_wnd, x, y); }
