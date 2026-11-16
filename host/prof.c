/* Sampling profiler for the recompiled game (build with -DX86_PROF).
 * Lifted functions store their guest address in x86_prof_cur on entry and after every call returns, so
 * the variable always names the innermost guest function running (host APIs are charged to their
 * caller). A thread on another core samples it every ~0.5 ms and logs the top functions periodically. */
#include "host.h"
#include <SDL.h>

#ifdef X86_PROF
volatile uint32_t x86_prof_cur;
#define HN 16384
static uint32_t hkey[HN], hcnt[HN];
static uint32_t total;
static SDL_mutex *mx;

static void count(uint32_t a) {
    uint32_t h = (a * 2654435761u) >> 18;
    for (;;) {
        if (hkey[h] == a) { hcnt[h]++; return; }
        if (!hkey[h]) { hkey[h] = a; hcnt[h] = 1; return; }
        h = (h + 1) & (HN - 1);
    }
}
static void report(void) {
    if (!total) return;
    hlog("PROFILE %u samples (top guest functions; 1 = idle/GIL released, 2 = host after wake)\n", total);
    for (int k = 0; k < 30; k++) {
        int best = -1;
        for (int i = 0; i < HN; i++) if (hkey[i] && hcnt[i] && (best < 0 || hcnt[i] > hcnt[best])) best = i;
        if (best < 0) break;
        hlog("  %6.2f%%  0x%06x\n", hcnt[best] * 100.0 / total, hkey[best]);
        hcnt[best] = 0;
    }
    memset(hkey, 0, sizeof hkey); memset(hcnt, 0, sizeof hcnt); total = 0;
}
static int sampler(void *arg) {
    (void)arg;
    uint32_t last = SDL_GetTicks();
    for (;;) {
#ifdef __vita__
        extern int sceKernelDelayThread(unsigned int usec);
        sceKernelDelayThread(500);
#else
        SDL_Delay(1);
#endif
        uint32_t a = x86_prof_cur;
        count(a ? a : 3); total++;
        if (SDL_GetTicks() - last >= 10000) { report(); last = SDL_GetTicks(); }
    }
    return 0;
}
void host_prof_start(void) {
    mx = SDL_CreateMutex();
    SDL_DetachThread(SDL_CreateThread(sampler, "prof", NULL));
    hlog("profiler: sampling guest functions\n");
}
void host_prof_mark(uint32_t v) { x86_prof_cur = v; }
#else
void host_prof_start(void) {}
void host_prof_mark(uint32_t v) { (void)v; }
#endif
