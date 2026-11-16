/* Portable MMX semantics on uint64_t. Hot blitters will get native NEON replacements later. */
#ifndef X86MMX_H
#define X86MMX_H
#include <stdint.h>

#define LANES(T, N, a, b, expr) do {                                   \
        T _x[N], _y[N], _r[N];                                         \
        memcpy(_x, &(a), 8); memcpy(_y, &(b), 8);                      \
        for (int _i = 0; _i < N; _i++) { T x = _x[_i], y = _y[_i]; _r[_i] = (T)(expr); } \
        uint64_t _o; memcpy(&_o, _r, 8); return _o; } while (0)

static inline int32_t sat_s(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline uint64_t mmx_pxor(uint64_t a, uint64_t b) { return a ^ b; }
static inline uint64_t mmx_pand(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mmx_por(uint64_t a, uint64_t b) { return a | b; }

static inline uint64_t mmx_paddb(uint64_t a, uint64_t b) { LANES(uint8_t, 8, a, b, x + y); }
static inline uint64_t mmx_paddw(uint64_t a, uint64_t b) { LANES(uint16_t, 4, a, b, x + y); }
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b) { LANES(uint32_t, 2, a, b, x + y); }
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b) { LANES(uint8_t, 8, a, b, x - y); }
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b) { LANES(uint16_t, 4, a, b, x - y); }
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b) { LANES(uint32_t, 2, a, b, x - y); }
static inline uint64_t mmx_paddusb(uint64_t a, uint64_t b) { LANES(uint8_t, 8, a, b, x + y > 255 ? 255 : x + y); }
static inline uint64_t mmx_paddusw(uint64_t a, uint64_t b) { LANES(uint16_t, 4, a, b, x + y > 65535 ? 65535 : x + y); }
static inline uint64_t mmx_psubusb(uint64_t a, uint64_t b) { LANES(uint8_t, 8, a, b, x > y ? x - y : 0); }
static inline uint64_t mmx_psubusw(uint64_t a, uint64_t b) { LANES(uint16_t, 4, a, b, x > y ? x - y : 0); }
static inline uint64_t mmx_paddsb(uint64_t a, uint64_t b) { LANES(int8_t, 8, a, b, sat_s(x + y, -128, 127)); }
static inline uint64_t mmx_paddsw(uint64_t a, uint64_t b) { LANES(int16_t, 4, a, b, sat_s(x + y, -32768, 32767)); }
static inline uint64_t mmx_psubsb(uint64_t a, uint64_t b) { LANES(int8_t, 8, a, b, sat_s(x - y, -128, 127)); }
static inline uint64_t mmx_psubsw(uint64_t a, uint64_t b) { LANES(int16_t, 4, a, b, sat_s(x - y, -32768, 32767)); }
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) { LANES(int16_t, 4, a, b, (int32_t)x * y); }
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) { LANES(int16_t, 4, a, b, ((int32_t)x * y) >> 16); }
static inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b) { LANES(uint8_t, 8, a, b, x == y ? 0xff : 0); }
static inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b) { LANES(uint16_t, 4, a, b, x == y ? 0xffff : 0); }
static inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b) { LANES(uint32_t, 2, a, b, x == y ? 0xffffffffu : 0); }
static inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b) { LANES(int8_t, 8, a, b, x > y ? -1 : 0); }
static inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b) { LANES(int16_t, 4, a, b, x > y ? -1 : 0); }
static inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b) { LANES(int32_t, 2, a, b, x > y ? -1 : 0); }

static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    int16_t x[4], y[4]; int32_t r[2]; memcpy(x, &a, 8); memcpy(y, &b, 8);
    r[0] = (int32_t)x[0] * y[0] + (int32_t)x[1] * y[1];
    r[1] = (int32_t)x[2] * y[2] + (int32_t)x[3] * y[3];
    uint64_t o; memcpy(&o, r, 8); return o;
}

/* shifts: count is the full 64-bit source (register/memory) or an imm8 */
#define MMX_SHIFT(name, T, N, op, maxv)                                                  \
    static inline uint64_t mmx_##name(uint64_t a, uint64_t c) {                         \
        T x[N]; memcpy(x, &a, 8);                                                        \
        for (int i = 0; i < N; i++) x[i] = (c > (maxv)) ? (T)(op##_SAT(x[i])) : (T)(x[i] op##_OP (int)c); \
        uint64_t o; memcpy(&o, x, 8); return o; }
#define SHR_OP >>
#define SHL_OP <<
#define SHR_SAT(v) 0
#define SHL_SAT(v) 0
#define SAR_OP >>
#define SAR_SAT(v) ((v) < 0 ? -1 : 0)
MMX_SHIFT(psrlw, uint16_t, 4, SHR, 15)
MMX_SHIFT(psrld, uint32_t, 2, SHR, 31)
MMX_SHIFT(psllw, uint16_t, 4, SHL, 15)
MMX_SHIFT(pslld, uint32_t, 2, SHL, 31)
MMX_SHIFT(psraw, int16_t, 4, SAR, 15)
MMX_SHIFT(psrad, int32_t, 2, SAR, 31)
static inline uint64_t mmx_psrlq(uint64_t a, uint64_t c) { return c > 63 ? 0 : a >> c; }
static inline uint64_t mmx_psllq(uint64_t a, uint64_t c) { return c > 63 ? 0 : a << c; }

static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) {
    int16_t x[8]; uint8_t r[8]; memcpy(x, &a, 8); memcpy(x + 4, &b, 8);
    for (int i = 0; i < 8; i++) r[i] = (uint8_t)sat_s(x[i], 0, 255);
    uint64_t o; memcpy(&o, r, 8); return o;
}
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) {
    int16_t x[8]; int8_t r[8]; memcpy(x, &a, 8); memcpy(x + 4, &b, 8);
    for (int i = 0; i < 8; i++) r[i] = (int8_t)sat_s(x[i], -128, 127);
    uint64_t o; memcpy(&o, r, 8); return o;
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) {
    int32_t x[4]; int16_t r[4]; memcpy(x, &a, 8); memcpy(x + 2, &b, 8);
    for (int i = 0; i < 4; i++) r[i] = (int16_t)sat_s(x[i], -32768, 32767);
    uint64_t o; memcpy(&o, r, 8); return o;
}
/* unpack low/high halves, interleaving a (dest) and b (src) */
#define MMX_UNPACK(name, T, N, half)                                             \
    static inline uint64_t mmx_##name(uint64_t a, uint64_t b) {                  \
        T x[N], y[N], r[N]; memcpy(x, &a, 8); memcpy(y, &b, 8);                  \
        for (int i = 0; i < N / 2; i++) { r[2 * i] = x[i + (half) * N / 2]; r[2 * i + 1] = y[i + (half) * N / 2]; } \
        uint64_t o; memcpy(&o, r, 8); return o; }
MMX_UNPACK(punpcklbw, uint8_t, 8, 0)
MMX_UNPACK(punpcklwd, uint16_t, 4, 0)
MMX_UNPACK(punpckldq, uint32_t, 2, 0)
MMX_UNPACK(punpckhbw, uint8_t, 8, 1)
MMX_UNPACK(punpckhwd, uint16_t, 4, 1)
MMX_UNPACK(punpckhdq, uint32_t, 2, 1)
#endif
