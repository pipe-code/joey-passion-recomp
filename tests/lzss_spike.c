/* Spike: run the lifted LZSS decoder (guest 0x54a000) over every compressed entry of Data.dat
 * and compare with reference files unpacked by tools/kcej.py.
 * Usage: lzss_spike <Data.dat> <ref_dir> <image.bin> */
#include "x86rt.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void lifted_register_all(void);
void hle_crt_register(void);
void f_54a000(CPU *ctx);

#define ARENA_SIZE 0x08000000u   /* 128 MB guest space */
#define STACK_TOP  0x01f00000u
#define HEAP_BASE  0x02000000u
static uint32_t heap_ptr = HEAP_BASE;

static uint32_t guest_alloc(uint32_t n) {
    uint32_t p = (heap_ptr + 15) & ~15u;
    heap_ptr = p + n;
    if (heap_ptr > ARENA_SIZE) x86_fatal("guest heap exhausted", n);
    return p;
}

/* HLE override for guest 0x54a1d0 (operator new / malloc, cdecl) */
static int hle_alloc_calls;
static void hle_malloc(CPU *ctx) {
    ctx->eax = guest_alloc(ARG(ctx, 0));
    hle_alloc_calls++;
    RET_CDECL(ctx);
}

/* Native reference decoder (same algorithm) for speed comparison */
static void native_lzss(const uint8_t *src, uint8_t *dst, uint32_t usize) {
    uint8_t win[4096] = {0}; uint32_t r = 0xfee, flags = 0;
    while (usize) {
        flags >>= 1;
        if (!(flags & 0x100)) flags = *src++ | 0xff00;
        if (flags & 1) { uint8_t c = *src++; *dst++ = c; win[r] = c; r = (r + 1) & 0xfff; usize--; }
        else {
            uint32_t p = src[0] | ((src[1] & 0xf0) << 4), l = (src[1] & 0xf) + 3; src += 2;
            for (uint32_t k = 0; k < l && usize; k++) { uint8_t c = win[(p + k) & 0xfff]; *dst++ = c; win[r] = c; r = (r + 1) & 0xfff; usize--; }
        }
    }
}

static uint8_t *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len ? *len : 1); fread(b, 1, *len, f); fclose(f); return b;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s Data.dat ref_dir image.bin\n", argv[0]); return 2; }
    MEM = calloc(1, ARENA_SIZE);
    long ilen; uint8_t *img = read_file(argv[3], &ilen);
    memcpy(MEM + IMAGE_BASE, img, ilen);
    lifted_register_all();
    hle_crt_register();
    x86_register(0x54a1d0u, hle_malloc);

    FILE *dat = fopen(argv[1], "rb");
    char magic[8]; uint32_t n; fread(magic, 1, 8, dat); fread(&n, 4, 1, dat);
    uint8_t *idx = malloc(n * 0x10c); fread(idx, 0x10c, n, dat);

    int tested = 0, ok = 0; double t_lift = 0, t_nat = 0; uint64_t bytes = 0;
    uint32_t this_obj = guest_alloc(0x1100);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = idx + i * 0x10c; char name[257]; uint32_t off, usize, csize;
        for (int k = 0; k < 256; k++) { uint8_t c = e[k]; name[k] = (char)((c >> 4) | (c << 4)); }
        name[256] = 0;
        for (int k = 0; k < 256; k++) if (name[k] == '\\') name[k] = '/';
        memcpy(&off, e + 0x100, 4); memcpy(&usize, e + 0x104, 4); memcpy(&csize, e + 0x108, 4);
        if (usize == csize) continue;

        uint32_t heap_mark = heap_ptr;
        uint32_t src = guest_alloc(csize + 16);
        fseek(dat, off, SEEK_SET); fread(MEM + src, 1, csize, dat);
        uint32_t outpp = guest_alloc(4);
        int use_alloc = (tested & 1);              /* alternate: game allocates vs. caller buffer */
        if (!use_alloc) WR32(outpp, guest_alloc(usize));

        CPU ctx = {0};
        ctx.esp = STACK_TOP;
        ctx.esp -= 4; WR32(ctx.esp, use_alloc);    /* arg3: allocate? */
        ctx.esp -= 4; WR32(ctx.esp, usize);        /* arg2: output size */
        ctx.esp -= 4; WR32(ctx.esp, outpp);        /* arg1: uint8_t** out */
        ctx.esp -= 4; WR32(ctx.esp, src);          /* arg0: src */
        ctx.esp -= 4; WR32(ctx.esp, 0xdeadbeefu);  /* return address */
        ctx.ecx = this_obj; ctx.ebx = 0x11111111; ctx.esi = 0x22222222; ctx.edi = 0x33333333; ctx.ebp = 0x44444444;

        double t0 = now();
        f_54a000(&ctx);
        t_lift += now() - t0;

        int good = ctx.esp == STACK_TOP && ctx.eax == 0 && ctx.ebx == 0x11111111 && ctx.esi == 0x22222222 &&
                   ctx.edi == 0x33333333 && ctx.ebp == 0x44444444;
        char ref[1024]; snprintf(ref, sizeof ref, "%s/%s", argv[2], name);
        long rlen; uint8_t *rb = read_file(ref, &rlen);
        good = good && rb && rlen == (long)usize && memcmp(rb, MEM + RD32(outpp), usize) == 0;

        uint8_t *nat = malloc(usize);
        t0 = now(); native_lzss(MEM + src, nat, usize); t_nat += now() - t0;
        good = good && memcmp(nat, MEM + RD32(outpp), usize) == 0;
        free(nat); free(rb);

        tested++; ok += good; bytes += usize;
        if (!good) printf("MISMATCH %s (esp=%08x eax=%08x)\n", name, ctx.esp, ctx.eax);
        heap_ptr = heap_mark;
    }
    printf("compressed entries: %d, identical: %d, HLE malloc calls: %d\n", tested, ok, hle_alloc_calls);
    printf("decoded %.1f MB | lifted %.1f ms (%.0f MB/s) | native %.1f ms (%.0f MB/s) | slowdown x%.2f\n",
           bytes / 1e6, t_lift * 1e3, bytes / 1e6 / t_lift, t_nat * 1e3, bytes / 1e6 / t_nat, t_lift / t_nat);
    return ok == tested ? 0 : 1;
}
