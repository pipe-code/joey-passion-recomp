/* Guest threads on SDL threads with a global interpreter lock (GIL): only one guest thread runs at a time.
 * The GIL is released while blocking (Sleep, waits, busy critical sections, GetMessage, vsync present).
 * Waiting uses one condition variable tied to the GIL; any state change that may wake waiters broadcasts it. */
#include "host.h"
#include <SDL.h>
#include <setjmp.h>

uint32_t host_ticks(void);
int handle_type(uint32_t h);

typedef struct GThread {
    CPU cpu;
    uint32_t id, teb, start, param, stack_lo, stack_hi, exit_code;
    int started, finished, suspended;
    uint32_t tls[64];
    jmp_buf exit_jmp;
    SDL_Thread *sdl;
    struct GThread *next_live;      /* host threads not yet joined (see reap_threads) */
} GThread;

static SDL_mutex *gil;
static SDL_cond *sync_cond;
static SDL_TLSID cur_tls;
static GThread main_thread;
static uint32_t next_tid = 2, tls_used[64];

static GThread *cur(void) { GThread *t = SDL_TLSGet(cur_tls); return t ? t : &main_thread; }
void host_prof_mark(uint32_t v);
/* The GIL mutex is not fair: a thread that unlocks and relocks right away usually wins again. When the
 * game thread is CPU-bound (frames slower than its 33 ms budget) it never sleeps, and the game's music
 * streaming thread would starve: the DirectSound ring buffer then replays stale audio ("scratched
 * record"). gil_yield() hands the GIL over explicitly whenever another guest thread is waiting. */
static SDL_atomic_t gil_waiters, gil_handoffs;
static SDL_mutex *cond_mx;          /* protects sync_cond waits (the GIL is released while waiting) */
void gil_release(void) { host_prof_mark(1); SDL_UnlockMutex(gil); }
void gil_acquire(void) {
    SDL_AtomicAdd(&gil_waiters, 1);
    SDL_LockMutex(gil);
    SDL_AtomicAdd(&gil_waiters, -1);
    SDL_AtomicAdd(&gil_handoffs, 1);
    host_prof_mark(2);
}
void gil_yield(void) {
    if (SDL_AtomicGet(&gil_waiters) <= 0) return;
    int h = SDL_AtomicGet(&gil_handoffs);
    gil_release();
    for (int spins = 0; SDL_AtomicGet(&gil_handoffs) == h && spins < 2000; spins++) SDL_Delay(0);
    gil_acquire();
}
static void wake_all(void) { SDL_LockMutex(cond_mx); SDL_CondBroadcast(sync_cond); SDL_UnlockMutex(cond_mx); }
/* wait (GIL released) until woken or `ms` elapses; returns remaining budget semantics left to caller */
static void wait_some(uint32_t ms) {
    int m = cur() == &main_thread; double t = m ? prof_now() : 0;
    SDL_LockMutex(cond_mx);         /* taken before the GIL is dropped: no lost wakeups */
    gil_release();
    SDL_CondWaitTimeout(sync_cond, cond_mx, ms);
    SDL_UnlockMutex(cond_mx);
    gil_acquire();
    if (m) prof_acc[PROF_WAIT] += prof_now() - t;
}

void host_threads_init(void) {
    gil = SDL_CreateMutex(); sync_cond = SDL_CreateCond(); cond_mx = SDL_CreateMutex(); cur_tls = SDL_TLSCreate();
    main_thread.id = 1; main_thread.teb = TEB_ADDR;
    SDL_LockMutex(gil);
}

/* ---------------------------------------------------------------- threads */
static int thread_main(void *arg) {
    GThread *t = arg;
    SDL_TLSSet(cur_tls, t, NULL);
    gil_acquire();
    if (!setjmp(t->exit_jmp)) {
        x86_call(&t->cpu, t->start);          /* LPTHREAD_START_ROUTINE is stdcall(param) */
        t->exit_code = t->cpu.eax;
    }
    t->finished = 1;
    /* the guest stack (1 MB by default) and TEB are no longer used: give them back to the guest heap
     * (the game spawns a loader thread per screen; leaking them added up to megabytes per duel) */
    gfree_aligned(t->stack_lo); gfree_aligned(t->teb);
    t->stack_lo = t->teb = 0;
    hlog("thread %u finished (code %u)\n", t->id, t->exit_code);
    wake_all();
    SDL_UnlockMutex(gil);
    return 0;
}
/* Join host threads whose guest code has finished. They are not detached: SDL's Vita backend never deletes a
 * detached thread, so each one leaked its 2 MB stack and a long session (one loader thread per screen) ran
 * the Vita out of memory until thread creation failed and the game froze. */
static GThread *live_threads;
static void reap_threads(void) {
    for (GThread **pp = &live_threads; *pp; ) {
        GThread *t = *pp;
        if (t->finished) { SDL_WaitThread(t->sdl, NULL); t->sdl = NULL; *pp = t->next_live; }
        else pp = &t->next_live;
    }
}
static void start_thread(GThread *t) {
    if (t->started) return;
    t->started = 1;
    reap_threads();
    char name[16]; snprintf(name, sizeof name, "guest%u", t->id);
    t->sdl = SDL_CreateThreadWithStackSize(thread_main, name, 2u * 1024 * 1024, t);   /* deep C nesting */
    if (!t->sdl) t->sdl = SDL_CreateThreadWithStackSize(thread_main, name, 1024 * 1024, t);
    if (!t->sdl) { hlog("CreateThread: host thread for tid %u failed: %s\n", t->id, SDL_GetError()); return; }
    t->next_live = live_threads; live_threads = t;
}
API(CreateThread, 6) {          /* attr, stacksize, start, param, flags, tid* */
    GThread *t = calloc(1, sizeof *t);
    uint32_t ss = A(1) ? (A(1) + 0xFFFF) & ~0xFFFFu : 0x100000u;
    t->id = next_tid++; t->start = A(2); t->param = A(3);
    t->stack_lo = galloc_aligned(ss, 0x10000); t->stack_hi = t->stack_lo + ss;
    t->teb = galloc_aligned(0x1000, 0x1000);
    WR32(t->teb + 0x00, 0xFFFFFFFFu); WR32(t->teb + 0x04, t->stack_hi); WR32(t->teb + 0x08, t->stack_lo);
    WR32(t->teb + 0x18, t->teb); WR32(t->teb + 0x20, 1); WR32(t->teb + 0x24, t->id); WR32(t->teb + 0x30, PEB_ADDR);
    t->cpu.esp = t->stack_hi - 16;
    t->cpu.fs_base = t->teb; t->cpu.fcw = 0x27F;
    t->cpu.esp -= 4; WR32(t->cpu.esp, t->param);
    t->cpu.esp -= 4; WR32(t->cpu.esp, 0x7FFFFFF0u);     /* return magic */
    uint32_t h = handle_new(H_THREAD, t);
    if (A(5)) WR32(A(5), t->id);
    hlog("CreateThread(start 0x%x, param 0x%x, flags 0x%x) -> tid %u\n", A(2), A(3), A(4), t->id);
    if (A(4) & 4) t->suspended = 1; else start_thread(t);
    return h;
}
API(ResumeThread, 1) {
    GThread *t = handle_get(A(0), H_THREAD);
    if (!t) return 0xFFFFFFFFu;
    uint32_t prev = (uint32_t)t->suspended;
    if (t->suspended) { t->suspended = 0; start_thread(t); }
    return prev;
}
API(ExitThread, 1) {
    GThread *t = cur();
    if (t == &main_thread) { hlog("ExitThread on main thread\n"); exit((int)A(0)); }
    t->exit_code = A(0);
    longjmp(t->exit_jmp, 1);
}
API(GetCurrentThreadId, 0) { return cur()->id; }

/* ---------------------------------------------------------------- TLS */
API(TlsAlloc, 0) { for (int i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; return (uint32_t)i; } return 0xFFFFFFFFu; }
API(TlsGetValue, 1) { last_error = 0; return A(0) < 64 ? cur()->tls[A(0)] : 0; }
API(TlsSetValue, 2) { if (A(0) >= 64) return 0; cur()->tls[A(0)] = A(1); return 1; }

/* ---------------------------------------------------------------- critical sections (guest struct: +8 recursion, +12 owner) */
API(InitializeCriticalSection, 1) { memset(GPTR(A(0)), 0, 24); WR32(A(0) + 4, 0xFFFFFFFFu); return 0; }
API(DeleteCriticalSection, 1) { return 0; }
API(EnterCriticalSection, 1) {
    uint32_t cs = A(0), me = cur()->id;
    while (RD32(cs + 12) && RD32(cs + 12) != me) wait_some(10);
    WR32(cs + 12, me); WR32(cs + 8, RD32(cs + 8) + 1);
    return 0;
}
API(LeaveCriticalSection, 1) {
    uint32_t cs = A(0), n = RD32(cs + 8);
    if (n) { WR32(cs + 8, n - 1); if (n == 1) { WR32(cs + 12, 0); wake_all(); } }
    return 0;
}

/* ---------------------------------------------------------------- events, mutexes, waits */
typedef struct { int signaled, manual; } HEvent;
typedef struct { uint32_t owner, count; } HMutex;
API(CreateEventA, 4) { HEvent *e = calloc(1, sizeof *e); e->manual = (int)A(1); e->signaled = (int)A(2); return handle_new(H_EVENT, e); }
API(SetEvent, 1) { HEvent *e = handle_get(A(0), H_EVENT); if (!e) return 0; e->signaled = 1; wake_all(); return 1; }
API(CreateMutexA, 3) { HMutex *m = calloc(1, sizeof *m); if (A(1)) { m->owner = cur()->id; m->count = 1; } return handle_new(H_MUTEX, m); }
API(ReleaseMutex, 1) {
    HMutex *m = handle_get(A(0), H_MUTEX);
    if (!m || m->owner != cur()->id) return 0;
    if (--m->count == 0) { m->owner = 0; wake_all(); }
    return 1;
}
static int try_acquire(uint32_t h) {
    switch (handle_type(h)) {
    case H_EVENT: { HEvent *e = handle_get(h, H_EVENT); if (!e->signaled) return 0; if (!e->manual) e->signaled = 0; return 1; }
    case H_MUTEX: { HMutex *m = handle_get(h, H_MUTEX); uint32_t me = cur()->id;
        if (m->owner && m->owner != me) return 0; m->owner = me; m->count++; return 1; }
    case H_THREAD: return ((GThread *)handle_get(h, H_THREAD))->finished;
    default: return 1;
    }
}
API(WaitForSingleObject, 2) {   /* handle, ms -> WAIT_OBJECT_0 / WAIT_TIMEOUT / WAIT_FAILED */
    uint32_t h = A(0), ms = A(1), t0 = host_ticks();
    if (handle_type(h) == H_FREE) { last_error = 6; return 0xFFFFFFFFu; }
    for (;;) {
        if (try_acquire(h)) return 0;
        uint32_t el = host_ticks() - t0;
        if (ms != 0xFFFFFFFFu && el >= ms) return 0x102;
        wait_some(ms == 0xFFFFFFFFu ? 50 : (ms - el < 50 ? ms - el : 50));
    }
}
void host_pump_events(void);
API(Sleep, 1) {
    int m = cur() == &main_thread;
    if (m) host_pump_events();
    double t = prof_now();
    gil_release();
    SDL_Delay(A(0));
    gil_acquire();
    if (m) prof_acc[PROF_SLEEP] += prof_now() - t;
    return 0;
}

const HostExport threads_exports[] = {
#define X(n) {#n, w_##n},
    X(CreateThread) X(ResumeThread) X(ExitThread) X(GetCurrentThreadId) X(TlsAlloc) X(TlsGetValue) X(TlsSetValue)
    X(InitializeCriticalSection) X(DeleteCriticalSection) X(EnterCriticalSection) X(LeaveCriticalSection)
    X(CreateEventA) X(SetEvent) X(CreateMutexA) X(ReleaseMutex) X(WaitForSingleObject) X(Sleep)
#undef X
    {NULL, NULL}};
