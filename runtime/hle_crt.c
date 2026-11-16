/* Native replacements (HLE) for hot or hand-written-asm CRT routines in Joey_Pc.exe. */
#include "x86rt.h"

static void hle_memmove(CPU *ctx) {          /* void *memmove(void *dst, const void *src, size_t n), cdecl */
    uint32_t dst = ARG(ctx, 0), src = ARG(ctx, 1), n = ARG(ctx, 2);
    memmove(MEM + dst, MEM + src, n);
    ctx->eax = dst;
    RET_CDECL(ctx);
}

void hle_crt_register(void) {
    x86_register(0x5c38c0u, hle_memmove);
    x86_register(0x5c6110u, hle_memmove);
}
