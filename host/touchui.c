/* Android touch layer: the 4:3 game in the middle of the screen, buttons in the side bars.
 *
 *   left bar:  Menu (Esc), Keyboard (reopens the text field keyboard; dim when no field is active), Right click
 *   right bar: scroll strip (tap the upper / lower half = one wheel notch, hold = repeat, drag = scroll)
 *
 * Game area: a finger moves the cursor (the game shows hover info, e.g. card descriptions, while it moves);
 * lifting it where it went down is a click, lifting it after sliding away is just a look. Bars are at least
 * MIN_BAR_IN wide: on screens that are close to 4:3 (tablets) the game shrinks a little to make room.
 * video.c draws through touchui_render(); user32.c routes SDL finger events here. */
#include "host.h"
#ifdef __ANDROID__
#include <SDL.h>
#include <SDL_ttf.h>
#include <math.h>

#define MIN_BAR_IN   0.55f       /* inches */
#define TAP_SLOP_IN  0.12f       /* finger travel that turns a tap into a look */
#define SCROLL_IN    0.22f       /* drag per wheel notch */
#define SMALL_GAME_IN 3.0f       /* game image shorter than this (inches): small text gets +1 px, as on the Vita */

void host_touch_hover(int x, int y), host_touch_click(int x, int y), host_right_click(void), host_escape(void);
void host_wheel(int notches), host_text_reopen(void);
int host_text_field_active(void);
SDL_Window *host_main_sdl_window(void);

enum { B_MENU, B_KEYB, B_RCLICK, B_SCROLL, NB };
static SDL_Rect game, btn[NB];
static int W, H, GW = 800, GH = 600, laid, small_screen = -1;
static float dpi = 160;
static SDL_Texture *label[NB];
static int label_w[NB], label_h[NB];
static int pressed[NB];

typedef struct { SDL_FingerID id; int used, target; float x0, y0, lasty; int moved, repeated; uint32_t next_repeat; } Finger;
static Finger fingers[10];
#define T_GAME (-1)

static int es(void) {
    static int v = -1;
    if (v < 0) {
        v = 0;
        SDL_Locale *l = SDL_GetPreferredLocales();
        if (l) { v = l[0].language && !strcmp(l[0].language, "es"); SDL_free(l); }
    }
    return v;
}

static void layout(int w, int h) {
    W = w; H = h;
    float hd = 0, vd = 0;
    if (SDL_GetDisplayDPI(0, NULL, &hd, &vd) == 0 && hd > 50) dpi = hd;
    int minbar = (int)(MIN_BAR_IN * dpi);
    int gh = H, gw = gh * GW / GH;
    if (gw > W - 2 * minbar) { gw = W - 2 * minbar; gh = gw * GH / GW; }
    if (gw < W / 2) { gw = W; gh = gw * GH / GW; if (gh > H) { gh = H; gw = gh * GW / GH; } }   /* tiny/odd screens */
    game = (SDL_Rect){(W - gw) / 2, (H - gh) / 2, gw, gh};
    float vdi = vd > 50 ? vd : dpi;
    small_screen = gh / vdi < SMALL_GAME_IN;
    int bar = game.x;
    int s = (int)fminf(bar * 0.72f, 0.8f * dpi);
    int bh = (int)fminf(s * 0.6f, H * 0.2f);
    btn[B_MENU]   = (SDL_Rect){(bar - s) / 2, H * 18 / 100 - bh / 2, s, bh};
    btn[B_KEYB]   = (SDL_Rect){(bar - s) / 2, H * 42 / 100 - bh / 2, s, bh};
    btn[B_RCLICK] = (SDL_Rect){(bar - s) / 2, H * 72 / 100 - bh / 2, s, bh};
    int sw = s * 7 / 10;
    btn[B_SCROLL] = (SDL_Rect){W - bar + (bar - sw) / 2, H * 10 / 100, sw, H * 80 / 100};
    laid = 1;
    hlog("touchui: screen %dx%d, %.0f dpi, game %dx%d at %d,%d (%.2f in tall%s), bar %d px\n", W, H, dpi, gw, gh,
         game.x, game.y, gh / vdi, small_screen ? ", small text +1px" : "", bar);
}

/* small text +1 px (gdi32.c): the game image is about as small as on the Vita */
int host_small_screen(void) {
    if (small_screen < 0) {
        SDL_Window *w = host_main_sdl_window();
        int ww = 0, hh = 0;
        if (w) SDL_GetWindowSize(w, &ww, &hh);
        if (ww <= 0) { SDL_DisplayMode m; if (SDL_GetDesktopDisplayMode(0, &m) == 0) { ww = m.w; hh = m.h; } }
        if (ww < hh) { int t = ww; ww = hh; hh = t; }    /* landscape */
        if (ww > 0) layout(ww, hh); else return 0;
    }
    return small_screen;
}

void touchui_to_game(int *x, int *y) {
    if (!laid || game.w <= 0) return;
    *x = (*x - game.x) * GW / game.w; *y = (*y - game.y) * GH / game.h;
    if (*x < 0) *x = 0; if (*x > GW - 1) *x = GW - 1;
    if (*y < 0) *y = 0; if (*y > GH - 1) *y = GH - 1;
}

static int hit(int b, int x, int y) {
    SDL_Rect r = btn[b];
    int m = (int)(0.08f * dpi);       /* forgiving edges */
    return x >= r.x - m && x < r.x + r.w + m && y >= r.y - m && y < r.y + r.h + m;
}

int touchui_event(const SDL_Event *e) {
    if (!laid) return 0;
    int x = (int)(e->tfinger.x * W), y = (int)(e->tfinger.y * H);
    Finger *f = NULL;
    for (int i = 0; i < 10; i++) if (fingers[i].used && fingers[i].id == e->tfinger.fingerId) f = &fingers[i];
    if (e->type == SDL_FINGERDOWN) {
        if (f) f->used = 0;
        for (int i = 0; i < 10 && !f; i++) if (!fingers[i].used) f = &fingers[i];
        if (!f) return 1;
        *f = (Finger){e->tfinger.fingerId, 1, T_GAME, (float)x, (float)y, (float)y, 0, 0, 0};
        for (int b = 0; b < NB; b++) if (hit(b, x, y)) f->target = b;
        if (f->target == T_GAME) {
            for (int i = 0; i < 10; i++)      /* one finger drives the cursor */
                if (&fingers[i] != f && fingers[i].used && fingers[i].target == T_GAME) { f->used = 0; return 1; }
            if (x < game.x || x >= game.x + game.w) { f->target = NB; return 1; }   /* empty bar space */
            int gx = x, gy = y; touchui_to_game(&gx, &gy); host_touch_hover(gx, gy);
            return 1;
        }
        pressed[f->target] = 1;
        if (f->target == B_SCROLL) f->next_repeat = SDL_GetTicks() + 400;
        return 1;
    }
    if (!f) return 1;
    if (e->type == SDL_FINGERMOTION) {
        if (fabsf(x - f->x0) + fabsf(y - f->y0) > TAP_SLOP_IN * dpi) f->moved = 1;
        if (f->target == T_GAME) { int gx = x, gy = y; touchui_to_game(&gx, &gy); host_touch_hover(gx, gy); }
        else if (f->target == B_SCROLL) {
            float step = SCROLL_IN * dpi;
            while (y - f->lasty >= step) { host_wheel(1); f->lasty += step; f->moved = 1; }     /* drag down = back */
            while (f->lasty - y >= step) { host_wheel(-1); f->lasty -= step; f->moved = 1; }
        }
        return 1;
    }
    /* finger up */
    int t = f->target; f->used = 0;
    if (t == T_GAME) {
        int gx = x, gy = y; touchui_to_game(&gx, &gy);
        if (!f->moved) host_touch_click(gx, gy); else host_touch_hover(gx, gy);
        return 1;
    }
    if (t >= NB) return 1;
    pressed[t] = 0;
    if (!hit(t, x, y) && t != B_SCROLL) return 1;      /* slid off the button: cancel */
    switch (t) {
    case B_MENU: host_escape(); break;
    case B_KEYB: host_text_reopen(); break;
    case B_RCLICK: host_right_click(); break;
    case B_SCROLL:
        if (!f->moved && !f->repeated) host_wheel(f->y0 < btn[B_SCROLL].y + btn[B_SCROLL].h / 2 ? 1 : -1);
        break;
    }
    return 1;
}

/* held scroll strip: repeat */
void touchui_pump(void) {
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < 10; i++) {
        Finger *f = &fingers[i];
        if (!f->used || f->target != B_SCROLL || f->moved) continue;
        if ((int32_t)(now - f->next_repeat) < 0) continue;
        host_wheel(f->y0 < btn[B_SCROLL].y + btn[B_SCROLL].h / 2 ? 1 : -1);
        f->next_repeat = now + 110;
        f->repeated = 1;                    /* a held repeat is not also a tap on release */
    }
}

/* ------------------------------------------------------------------ drawing */
static void make_labels(SDL_Renderer *r) {
    if (!TTF_WasInit() && TTF_Init() != 0) return;
    const char *txt[NB] = { es() ? "Menú" : "Menu", es() ? "Teclado" : "Keyboard", es() ? "Clic der." : "Right click", NULL };
    int px = btn[B_MENU].h * 3 / 10; if (px < 10) px = 10;
    TTF_Font *font = NULL;
    for (; px >= 10; px -= 2) {             /* the longest label fits the button */
        if (font) TTF_CloseFont(font);
        font = TTF_OpenFont("fonts/Arimo.ttf", px);
        if (!font) return;
        TTF_SetFontStyle(font, TTF_STYLE_BOLD);
        int widest = 0;
        for (int b = 0; b < NB; b++) { int w = 0; if (txt[b] && TTF_SizeUTF8(font, txt[b], &w, NULL) == 0 && w > widest) widest = w; }
        if (widest <= btn[B_MENU].w * 85 / 100) break;
    }
    for (int b = 0; b < NB; b++) {
        if (label[b]) { SDL_DestroyTexture(label[b]); label[b] = NULL; }
        if (!txt[b]) continue;
        SDL_Surface *s = TTF_RenderUTF8_Blended(font, txt[b], (SDL_Color){235, 225, 200, 255});
        if (!s) continue;
        label[b] = SDL_CreateTextureFromSurface(r, s); label_w[b] = s->w; label_h[b] = s->h;
        SDL_FreeSurface(s);
    }
    TTF_CloseFont(font);
}

static void panel(SDL_Renderer *r, SDL_Rect b, int on, int dim) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, on ? 120 : 45, on ? 95 : 40, on ? 50 : 35, dim ? 90 : 210);
    SDL_RenderFillRect(r, &b);
    SDL_SetRenderDrawColor(r, 190, 150, 70, dim ? 80 : 230);
    for (int i = 0; i < 2; i++) { SDL_Rect o = {b.x + i, b.y + i, b.w - 2 * i, b.h - 2 * i}; SDL_RenderDrawRect(r, &o); }
}
static void triangle(SDL_Renderer *r, int cx, int cy, int s, int up, int on) {
    SDL_Color c = on ? (SDL_Color){255, 230, 150, 255} : (SDL_Color){220, 200, 160, 230};
    float d = up ? -1.f : 1.f;
    SDL_Vertex v[3] = {
        {{(float)cx, cy + d * s * 0.5f}, c, {0, 0}},
        {{cx - s * 0.6f, cy - d * s * 0.4f}, c, {0, 0}},
        {{cx + s * 0.6f, cy - d * s * 0.4f}, c, {0, 0}},
    };
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

void touchui_render(SDL_Renderer *r, SDL_Texture *tex, int tw, int th) {
    int w, h;
    SDL_GetRendererOutputSize(r, &w, &h);
    if (!laid || w != W || h != H || tw != GW || th != GH) {
        GW = tw; GH = th; layout(w, h);
        make_labels(r);
    }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_RenderCopy(r, tex, NULL, &game);
    if (game.x < (int)(0.3f * dpi)) return;          /* no room for buttons */
    int dimk = !host_text_field_active();
    for (int b = 0; b < B_SCROLL; b++) {
        panel(r, btn[b], pressed[b], b == B_KEYB && dimk);
        if (label[b]) {
            SDL_Rect d = {btn[b].x + (btn[b].w - label_w[b]) / 2, btn[b].y + (btn[b].h - label_h[b]) / 2, label_w[b], label_h[b]};
            SDL_SetTextureAlphaMod(label[b], b == B_KEYB && dimk ? 90 : 255);
            SDL_RenderCopy(r, label[b], NULL, &d);
        }
    }
    SDL_Rect sb = btn[B_SCROLL];
    panel(r, sb, 0, 0);
    int cx = sb.x + sb.w / 2, ts = sb.w / 2;
    int up_on = 0, dn_on = 0;
    for (int i = 0; i < 10; i++)
        if (fingers[i].used && fingers[i].target == B_SCROLL) { if (fingers[i].y0 < sb.y + sb.h / 2) up_on = 1; else dn_on = 1; }
    triangle(r, cx, sb.y + sb.h / 6, ts, 1, up_on);
    triangle(r, cx, sb.y + sb.h * 5 / 6, ts, 0, dn_on);
    SDL_SetRenderDrawColor(r, 190, 150, 70, 160);
    for (int i = 0; i < 5; i++) {                     /* grip lines in the middle */
        int yy = sb.y + sb.h / 2 + (i - 2) * sb.h / 30;
        SDL_RenderDrawLine(r, sb.x + sb.w / 4, yy, sb.x + sb.w * 3 / 4, yy);
    }
}
#else
int host_small_screen(void) {
#ifdef __vita__
    return 1;
#else
    return 0;
#endif
}
#endif
