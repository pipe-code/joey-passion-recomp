/* Host side of the recompiled game: a small Win32 emulation layer over the lifted code. */
#ifndef HOST_H
#define HOST_H
#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#ifdef _WIN32
/* Windows build (mingw-w64): the few POSIX spellings the host layer uses */
#include <io.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* ---- guest address space layout ---- */
extern uint32_t guest_space;        /* size of the guest address space (platform dependent) */
#define PEB_ADDR      0x00010000u
#define TEB_ADDR      0x00011000u   /* main thread TEB; others follow every 0x1000 */
#define LOWMISC_ADDR  0x00020000u   /* static host-owned strings (command line, env...) */
#define IMAGE_LO      0x00400000u
#define STACK_LO      0x00B00000u   /* main thread stack: 1 MB */
#define STACK_HI      0x00C00000u
#define THUNK_BASE    0x7FF00000u   /* fake addresses for imports / host callbacks (never dereferenced) */
#define HEAP_LO       0x01000000u
#define HEAP_HI       guest_space

#define GPTR(a)   ((void *)(MEM + (a)))
#define GSTR(a)   ((a) ? (const char *)(MEM + (a)) : NULL)
#define A(n)      ARG(ctx, n)

/* Define a stdcall import implementation: API(Name, nargs) { ... return value; } */
#define API(name, n)                                                         \
    static uint32_t api_##name(CPU *ctx);                                    \
    void w_##name(CPU *ctx) { uint32_t r_ = api_##name(ctx); ctx->eax = r_; RET_STDCALL(ctx, n); } \
    static uint32_t api_##name(CPU *ctx)
/* cdecl (varargs) variant: caller cleans the stack */
#define API_CDECL(name)                                                      \
    static uint32_t api_##name(CPU *ctx);                                    \
    void w_##name(CPU *ctx) { uint32_t r_ = api_##name(ctx); ctx->eax = r_; RET_CDECL(ctx); } \
    static uint32_t api_##name(CPU *ctx)

typedef struct { const char *name; x86_fn fn; } HostExport;

void gil_release(void);
void gil_yield(void);      /* let a waiting guest thread run (fair handoff) */
void gil_acquire(void);

/* logging */
extern int host_verbose;
void hlog(const char *fmt, ...);
#define TRACE_API(...) do { if (host_verbose) hlog(__VA_ARGS__); } while (0)

/* guest heap (galloc.c) */
void     galloc_init(uint32_t lo, uint32_t hi);
uint32_t galloc(uint32_t size, int zero);
void     gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size, int zero);
uint32_t galloc_aligned(uint32_t size, uint32_t align);   /* VirtualAlloc regions */
int gfree_aligned(uint32_t p);

/* host -> guest calls (callbacks such as WndProc). Preserves all registers except eax. */
uint32_t guest_call(CPU *ctx, uint32_t fn, int nargs, ...);

/* handles */
enum { H_FREE, H_FILE, H_FIND, H_EVENT, H_MUTEX, H_THREAD, H_HEAP, H_MODULE, H_WINDOW, H_DC, H_GDIOBJ, H_REGKEY, H_OTHER };
uint32_t handle_new(int type, void *obj);
void    *handle_get(uint32_t h, int type);
void     handle_close(uint32_t h);

/* paths: guest Windows path -> host path (case-insensitive lookup under the game dir) */
extern char game_dir[1024];
int host_path(const char *gpath, char *out, size_t outsz);

/* misc state */
extern uint32_t last_error;
extern CPU main_cpu;
uint32_t guest_strdup(const char *s);      /* host string -> guest heap */
uint32_t thunk_for(const char *dll, const char *name);  /* fake guest address of a host export (GetProcAddress) */
uint32_t host_add_thunk(const char *full_name, x86_fn fn);  /* fn NULL -> "unimplemented" stub that reports the name */
const char *host_thunk_name(uint32_t addr);

/* COM objects (com.c): guest object = [vtbl][host handle]; methods are stdcall with `this` first */
typedef struct { const char *name; x86_fn fn; } ComMethod;
uint32_t com_vtbl(const char *iface, const ComMethod *m, int n);   /* NULL fn -> reporting stub */
uint32_t com_new(uint32_t vtbl, void *host);
void com_free(uint32_t self);          /* release the guest COM object and its handle */
void    *com_host(uint32_t self);
#define THIS_HOST(T) ((T *)com_host(A(0)))

/* per-DLL export tables (NULL-terminated) */
extern const HostExport kernel32_exports[], threads_exports[], user32_exports[], user32_gdi_exports[], gdi32_exports[], advapi32_exports[],
                        ole32_exports[], oleaut32_exports[], winmm_exports[], msacm32_exports[], dinput_exports[];
#endif

/* lightweight profiling: seconds accumulated per bucket, logged once every few seconds by video_present */
enum { PROF_BLIT, PROF_UPLOAD, PROF_PRESENT, PROF_SLEEP, PROF_WAIT, PROF_IO, PROF_TEXT, PROF_N };
extern double prof_acc[PROF_N];
double prof_now(void);
/* API whose time is charged to a profiling bucket */
#define API_PROF(name, n, bucket)                                            \
    static uint32_t api_##name(CPU *ctx);                                    \
    void w_##name(CPU *ctx) { double t_ = prof_now(); uint32_t r_ = api_##name(ctx); prof_acc[bucket] += prof_now() - t_; \
                              ctx->eax = r_; RET_STDCALL(ctx, n); }          \
    static uint32_t api_##name(CPU *ctx)

