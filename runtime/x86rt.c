#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

uint8_t *MEM;
TraceRec *x86_trace_buf;
uint32_t x86_trace_len, x86_trace_cap;

/* Sorted-on-demand dispatch table: guest address -> host function. */
typedef struct { uint32_t addr; x86_fn fn; } Entry;
static Entry *tab;
static int ntab, captab, sorted_ok;

static int cmp(const void *a, const void *b) {
    uint32_t x = ((const Entry *)a)->addr, y = ((const Entry *)b)->addr;
    return x < y ? -1 : x > y;
}

void x86_register(uint32_t addr, x86_fn fn) {
    for (int i = 0; i < ntab; i++)
        if (tab[i].addr == addr) { tab[i].fn = fn; return; }
    if (ntab == captab) { captab = captab ? captab * 2 : 1024; tab = realloc(tab, captab * sizeof *tab); }
    tab[ntab++] = (Entry){addr, fn};
    sorted_ok = 0;
}

/* Tests can catch fatal errors instead of aborting the process. */
jmp_buf *x86_fatal_env;
const char *x86_fatal_what;
uint32_t x86_fatal_addr;

void (*x86_fatal_hook)(const char *what, uint32_t addr);
void x86_fatal(const char *what, uint32_t addr) {
    x86_fatal_what = what; x86_fatal_addr = addr;
    if (x86_fatal_env) longjmp(*x86_fatal_env, 1);
    if (x86_fatal_hook) x86_fatal_hook(what, addr);
    fprintf(stderr, "x86rt fatal: %s at 0x%08x\n", what, addr);
    abort();
}

uint32_t x86_current_target;   /* destination of the dispatch in progress (used by generic host stubs) */

void x86_call(CPU *ctx, uint32_t target) {
    x86_current_target = target;
    if (!sorted_ok) { qsort(tab, ntab, sizeof *tab, cmp); sorted_ok = 1; }
    int lo = 0, hi = ntab - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid].addr == target) { tab[mid].fn(ctx); return; }
        if (tab[mid].addr < target) lo = mid + 1; else hi = mid - 1;
    }
    fprintf(stderr, "x86rt: call to unknown guest function 0x%08x (return address on stack: 0x%08x)\n",
            target, RD32(ctx->esp));
    x86_fatal("call to unknown guest function", target);
}

/* ---- x87 80-bit extended <-> double ---- */
double RDF80(uint32_t a) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, MEM + a, 8); memcpy(&se, MEM + a + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void WRF80(uint32_t a, double v) {
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0;
    double m = fabs(v);
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (m != 0.0) {
        int e; double fr = frexp(m, &e);          /* m = fr * 2^e, fr in [0.5,1) */
        mant = (uint64_t)ldexp(fr, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(MEM + a, &mant, 8); memcpy(MEM + a + 8, &se, 2);
}

/* ---- CPUID: present a Pentium II class CPU with MMX, no SSE (matches the binary's code paths) ---- */
void x86_cpuid(CPU *ctx, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    (void)ctx;
    if (*eax == 0) { *eax = 1; *ebx = 0x756e6547; *edx = 0x49656e69; *ecx = 0x6c65746e; } /* GenuineIntel */
    else if (*eax == 1) { *eax = 0x652; *ebx = 0; *ecx = 0; *edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23); }
    else { *eax = *ebx = *ecx = *edx = 0; }
}
