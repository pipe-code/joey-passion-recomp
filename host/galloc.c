/* Guest heap: boundary-tag allocator living inside guest memory.
 *
 * Block at guest address b (8-aligned):  [b+0] prev_size (valid only if previous block is free)
 *                                         [b+4] size | INUSE | PREV_INUSE   (size includes the 8-byte header)
 *                                         [b+8] payload ... (free blocks: next, prev links)
 * Memory above `top` is untouched wilderness. Bins are segregated free lists (host-side heads). */
#include "host.h"

#define INUSE 1u
#define PREV_INUSE 2u
#define MINBLK 16u
#define NBINS 208

static uint32_t heap_lo, heap_hi, top;
static uint32_t live_bytes;     /* bytes in allocated blocks (stats) */
static uint32_t bins[NBINS];

static inline uint32_t bsize(uint32_t b) { return RD32(b + 4) & ~7u; }
static inline uint32_t bflags(uint32_t b) { return RD32(b + 4) & 7u; }
static inline void set_hdr(uint32_t b, uint32_t size, uint32_t flags) { WR32(b + 4, size | flags); }
static inline uint32_t nxt(uint32_t b) { return RD32(b + 8); }
static inline uint32_t prv(uint32_t b) { return RD32(b + 12); }

static int bin_of(uint32_t size) {
    if (size < 1024) return size >> 3;                      /* 0..127 exact (8-byte steps) */
    int l = 31 - __builtin_clz(size);                       /* >= 10 */
    int i = 128 + (l - 10) * 4 + ((size >> (l - 2)) & 3);
    return i < NBINS ? i : NBINS - 1;
}

static void bin_insert(uint32_t b) {
    int i = bin_of(bsize(b));
    uint32_t h = bins[i];
    WR32(b + 8, h); WR32(b + 12, 0);
    if (h) WR32(h + 12, b);
    bins[i] = b;
}

static void bin_remove(uint32_t b) {
    uint32_t n = nxt(b), p = prv(b);
    if (p) WR32(p + 8, n); else bins[bin_of(bsize(b))] = n;
    if (n) WR32(n + 12, p);
}

static void set_next_prev_inuse(uint32_t b, int inuse) {
    uint32_t n = b + bsize(b);
    if (n >= top) return;
    uint32_t f = bflags(n);
    set_hdr(n, bsize(n), inuse ? (f | PREV_INUSE) : (f & ~PREV_INUSE));
    if (!inuse) WR32(n, bsize(b));      /* footer: prev_size of the following block */
}

void galloc_init(uint32_t lo, uint32_t hi) {
    heap_lo = (lo + 7) & ~7u; heap_hi = hi; top = heap_lo;
    memset(bins, 0, sizeof bins);
}

static uint32_t take(uint32_t b, uint32_t need) {
    uint32_t size = bsize(b), f = bflags(b) & PREV_INUSE;
    if (size - need >= MINBLK) {
        uint32_t r = b + need;
        set_hdr(r, size - need, PREV_INUSE);
        set_hdr(b, need, f | INUSE);
        bin_insert(r);
        set_next_prev_inuse(r, 0);
    } else {
        set_hdr(b, size, f | INUSE);
        set_next_prev_inuse(b, 1);
    }
    return b + 8;
}

uint32_t galloc(uint32_t size, int zero) {
    if (size > heap_hi - heap_lo) return 0;
    uint32_t need = (size + 8 + 7) & ~7u;
    if (need < MINBLK) need = MINBLK;
    for (int i = bin_of(need); i < NBINS; i++) {
        for (uint32_t b = bins[i]; b; b = nxt(b)) {
            if (bsize(b) >= need) {
                bin_remove(b);
                uint32_t p = take(b, need);
                if (zero) memset(GPTR(p), 0, bsize(p - 8) - 8);
                live_bytes += bsize(p - 8);
                return p;
            }
        }
    }
    /* carve from the wilderness; its memory is zero unless it was given back by a free at the top */
    if (top + need > heap_hi) {
        static int n;
        if (n++ < 20) hlog("galloc: OUT OF GUEST MEMORY (%u bytes requested, heap top %.1f MB of %.1f MB)\n",
                          size, (top - heap_lo) / 1048576.0, (heap_hi - heap_lo) / 1048576.0);
        return 0;
    }
    uint32_t b = top;              /* the block below `top` is always in use: frees merge into the wilderness */
    top += need;
    set_hdr(b, need, INUSE | PREV_INUSE);
    if (zero) memset(GPTR(b + 8), 0, need - 8);
    live_bytes += need;
    return b + 8;
}

void gfree(uint32_t p) {
    if (!p) return;
    uint32_t b = p - 8;
    if (b < heap_lo || b >= top || !(bflags(b) & INUSE)) { hlog("gfree: bad pointer 0x%08x\n", p); return; }
    uint32_t size = bsize(b), f = bflags(b);
    live_bytes -= size;
    /* coalesce with next */
    uint32_t n = b + size;
    if (n < top && !(bflags(n) & INUSE)) { bin_remove(n); size += bsize(n); }
    /* coalesce with previous */
    if (!(f & PREV_INUSE)) {
        uint32_t ps = RD32(b), pb = b - ps;
        bin_remove(pb);
        size += ps; b = pb; f = bflags(pb);
    }
    if (b + size == top) {          /* give back to the wilderness (scrub so future carves are zero) */
        memset(GPTR(b), 0, size);
        top = b;
        return;
    }
    set_hdr(b, size, f & PREV_INUSE);
    bin_insert(b);
    set_next_prev_inuse(b, 0);
}

uint32_t gsize(uint32_t p) { return p ? bsize(p - 8) - 8 : 0; }

uint32_t grealloc(uint32_t p, uint32_t size, int zero) {
    if (!p) return galloc(size, zero);
    uint32_t old = gsize(p);
    if (size <= old) return p;
    uint32_t q = galloc(size, zero);
    if (!q) return 0;
    memcpy(GPTR(q), GPTR(p), old);
    gfree(p);
    return q;
}

/* Aligned regions (VirtualAlloc): over-allocate, remember the raw block. */
#define MAXALIGNED 4096
static uint32_t al_ptr[MAXALIGNED], al_raw[MAXALIGNED], al_size[MAXALIGNED];
static int n_al;

uint32_t galloc_aligned(uint32_t size, uint32_t align) {
    uint32_t raw = galloc(size + align, 0);
    if (!raw) return 0;
    uint32_t p = (raw + align - 1) & ~(align - 1);
    memset(GPTR(p), 0, size);
    for (int i = 0; i < n_al; i++)
        if (!al_ptr[i]) { al_ptr[i] = p; al_raw[i] = raw; al_size[i] = size; return p; }
    if (n_al == MAXALIGNED) { gfree(raw); return 0; }
    al_ptr[n_al] = p; al_raw[n_al] = raw; al_size[n_al] = size; n_al++;
    return p;
}

/* Returns 1 if p was an aligned region (and frees it). */
int gfree_aligned(uint32_t p) {
    for (int i = 0; i < n_al; i++)
        if (al_ptr[i] == p) { gfree(al_raw[i]); al_ptr[i] = 0; return 1; }
    return 0;
}

/* Region containing p (for VirtualAlloc(MEM_COMMIT) inside a reservation / VirtualQuery). */
int galloc_region(uint32_t p, uint32_t *base, uint32_t *size) {
    for (int i = 0; i < n_al; i++)
        if (al_ptr[i] && p >= al_ptr[i] && p < al_ptr[i] + al_size[i]) { *base = al_ptr[i]; *size = al_size[i]; return 1; }
    return 0;
}

uint32_t galloc_top(void) { return top; }
uint32_t galloc_live(void) { return live_bytes; }
