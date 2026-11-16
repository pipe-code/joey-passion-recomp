/* Runtime for statically recompiled x86 (Joey_Pc.exe).
 * Guest memory is a flat 32-bit space: host pointer = MEM + guest address. */
#ifndef X86RT_H
#define X86RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t fs_base;            /* guest address of the TEB (fs:[0]) */
    uint32_t cf, zf, sf, of, pf; /* flags, only spilled across tail calls / fallthrough */
    uint32_t eflags_hi;          /* ID bit etc. for pushfd/popfd (CPUID detection) */
    double st[8];                /* x87 stack (53-bit precision, MSVC default) */
    uint32_t ftop;
    uint16_t fcw, fsw;
    uint64_t mm[8];              /* MMX registers (not aliased onto st[]) */
} CPU;

extern uint8_t *MEM;             /* MEM + guest_addr = host pointer */

/* Lifted code defines X86_MEMBASE to a function-local copy of MEM: as a global, MEM would be reloaded
 * after every guest store (a uint8_t store may alias anything), costing a load per memory access. */
#ifndef X86_MEMBASE
#define X86_MEMBASE MEM
#endif
static inline uint8_t  RD8_ (const uint8_t *m, uint32_t a) { return m[a]; }
static inline uint16_t RD16_(const uint8_t *m, uint32_t a) { uint16_t v; memcpy(&v, m + a, 2); return v; }
static inline uint32_t RD32_(const uint8_t *m, uint32_t a) { uint32_t v; memcpy(&v, m + a, 4); return v; }
static inline uint64_t RD64_(const uint8_t *m, uint32_t a) { uint64_t v; memcpy(&v, m + a, 8); return v; }
static inline void WR8_ (uint8_t *m, uint32_t a, uint8_t  v) { m[a] = v; }
static inline void WR16_(uint8_t *m, uint32_t a, uint16_t v) { memcpy(m + a, &v, 2); }
static inline void WR32_(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
static inline void WR64_(uint8_t *m, uint32_t a, uint64_t v) { memcpy(m + a, &v, 8); }
static inline double RDF32_(const uint8_t *m, uint32_t a) { float f; memcpy(&f, m + a, 4); return f; }
static inline double RDF64_(const uint8_t *m, uint32_t a) { double d; memcpy(&d, m + a, 8); return d; }
static inline void WRF32_(uint8_t *m, uint32_t a, double v) { float f = (float)v; memcpy(m + a, &f, 4); }
static inline void WRF64_(uint8_t *m, uint32_t a, double v) { memcpy(m + a, &v, 8); }
#define RD8(a)  RD8_ (X86_MEMBASE, (a))
#define RD16(a) RD16_(X86_MEMBASE, (a))
#define RD32(a) RD32_(X86_MEMBASE, (a))
#define RD64(a) RD64_(X86_MEMBASE, (a))
#define WR8(a, v)  WR8_ (X86_MEMBASE, (a), (v))
#define WR16(a, v) WR16_(X86_MEMBASE, (a), (v))
#define WR32(a, v) WR32_(X86_MEMBASE, (a), (v))
#define WR64(a, v) WR64_(X86_MEMBASE, (a), (v))
#define RDF32(a) RDF32_(X86_MEMBASE, (a))
#define RDF64(a) RDF64_(X86_MEMBASE, (a))
#define WRF32(a, v) WRF32_(X86_MEMBASE, (a), (v))
#define WRF64(a, v) WRF64_(X86_MEMBASE, (a), (v))

static inline uint32_t PARITY(uint32_t r) { return (~__builtin_popcount(r & 0xff)) & 1; }

/* ---- x87 ---- */
double RDF80(uint32_t a);
void WRF80(uint32_t a, double v);

#define FST(i) ctx->st[(ctx->ftop + (i)) & 7]
#define FPUSH(v) do { double _v = (v); ctx->ftop = (ctx->ftop - 1) & 7; ctx->st[ctx->ftop] = _v; } while (0)
#define FPOP() (ctx->ftop = (ctx->ftop + 1) & 7)

static inline void x87_cmp(CPU *ctx, double a, double b) {
    ctx->fsw &= ~0x4500u;
    if (a != a || b != b) ctx->fsw |= 0x4500u;
    else if (a < b) ctx->fsw |= 0x0100u;
    else if (a == b) ctx->fsw |= 0x4000u;
}
static inline uint16_t x87_sw(CPU *ctx) { return (uint16_t)((ctx->fsw & ~0x3800u) | ((ctx->ftop & 7) << 11)); }
static inline double x87_rint(CPU *ctx, double v) {
    switch ((ctx->fcw >> 10) & 3) {
    case 0: return nearbyint(v);   /* host default: round to nearest even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
/* truncate=1 for fisttp; otherwise rounding mode from the control word. Out of range -> "integer indefinite". */
static inline int64_t x87_toint(CPU *ctx, double v, int truncate, int bits) {
    double r = truncate ? trunc(v) : x87_rint(ctx, v);
    double lim = bits == 16 ? 32768.0 : bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (!(r >= -lim && r < lim)) return bits == 16 ? 0x8000 : bits == 32 ? (int64_t)0x80000000u : INT64_MIN;
    return (int64_t)r;
}

/* ---- misc ---- */
void x86_cpuid(CPU *ctx, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

/* Call any guest address: lifted function, host override (HLE) or import thunk.
 * The return address must already be pushed on the guest stack. */
void x86_call(CPU *ctx, uint32_t target);
extern uint32_t x86_current_target;
typedef void (*x86_fn)(CPU *);
void x86_register(uint32_t addr, x86_fn fn);   /* later registrations win (overrides) */
void x86_fatal(const char *what, uint32_t addr);

/* Helpers for host overrides (HLE): args start at [esp+4] */
#define ARG(ctx, n) RD32((ctx)->esp + 4 + 4 * (n))
#define RET_CDECL(ctx) ((ctx)->esp += 4)
#define RET_STDCALL(ctx, nargs) ((ctx)->esp += 4 + 4 * (nargs))

/* Optional per-instruction trace (only emitted by the lifter with LIFT_TRACE=1) */
typedef struct { uint32_t addr, r[8]; } TraceRec;
extern TraceRec *x86_trace_buf;
extern uint32_t x86_trace_len, x86_trace_cap;
static inline void x86_trace_rec(CPU *ctx, uint32_t a) {
    if (x86_trace_len < x86_trace_cap) {
        TraceRec *t = &x86_trace_buf[x86_trace_len++];
        t->addr = a; memcpy(t->r, &ctx->eax, 32);
    }
}

#include "x86mmx.h"
#endif
