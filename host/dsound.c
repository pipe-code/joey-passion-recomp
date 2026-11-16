/* DirectSound v1 (IDirectSound + IDirectSoundBuffer) on a software mixer driven by the SDL audio callback.
 * Buffer memory lives in guest memory so the game's Lock/Unlock writes land directly in it. */
#include "host.h"
#include <SDL.h>
#include <math.h>

#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBPLAY_LOOPING 0x1
#define DSBLOCK_FROMWRITECURSOR 0x1
#define DSBLOCK_ENTIREBUFFER 0x2
#define OUT_RATE 44100

typedef struct DSBuf {
    int refs, primary;
    uint32_t self, data, size, flags;
    int *data_refs;                 /* shared by duplicates (DuplicateSoundBuffer shares the sample data) */
    int rate, ch, bits, block;      /* format */
    int playing, loop;
    double pos;                     /* in frames */
    int32_t vol, pan; uint32_t freq;
    struct DSBuf *next;
} DSBuf;
typedef struct { int refs; } DSound;

static uint32_t vt_ds, vt_buf;
static DSBuf *bufs;                 /* list of live secondary buffers */
static SDL_AudioDeviceID dev;
static int mixed_frames_logged;

static float db_gain(int32_t v) { return v <= -10000 ? 0.f : powf(10.f, (float)v / 2000.f); }

static void mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    static int32_t acc[8192];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, (size_t)frames * 8);
    for (DSBuf *b = bufs; b; b = b->next) {
        if (!b->playing || !b->size || !b->block) continue;
        uint32_t nfr = b->size / (uint32_t)b->block;
        double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / OUT_RATE;
        float g = db_gain(b->vol);
        float gl = g * (b->pan > 0 ? db_gain(-b->pan) : 1.f), gr = g * (b->pan < 0 ? db_gain(b->pan) : 1.f);
        const uint8_t *d = (const uint8_t *)GPTR(b->data);
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)b->pos;
            if (f >= nfr) { if (b->loop) { b->pos -= nfr; f = (uint32_t)b->pos; } else { b->playing = 0; b->pos = 0; break; } }
            const uint8_t *s = d + (size_t)f * b->block;
            int l, r;
            if (b->bits == 16) { l = (int16_t)(s[0] | s[1] << 8); r = b->ch == 2 ? (int16_t)(s[2] | s[3] << 8) : l; }
            else { l = (s[0] - 128) << 8; r = b->ch == 2 ? (s[1] - 128) << 8 : l; }
            acc[2 * i] += (int32_t)(l * gl);
            acc[2 * i + 1] += (int32_t)(r * gr);
            b->pos += step;
        }
    }
    for (int i = 0; i < 2 * frames; i++) out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    mixed_frames_logged += frames;
    static FILE *dump; static int dump_init;
    if (!dump_init) { dump_init = 1; const char *p = getenv("JOEY_AUDIO_DUMP"); if (p) dump = fopen(p, "wb"); }
    if (dump) { fwrite(out, 4, (size_t)frames, dump); fflush(dump); }      /* raw s16le stereo 44100 */
}

static void audio_open(void) {
    if (dev) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { hlog("SDL audio: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { hlog("SDL_OpenAudioDevice: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
    hlog("audio: %d Hz, %d ch, %d samples\n", have.freq, have.channels, have.samples);
}
#define LOCK() do { if (dev) SDL_LockAudioDevice(dev); } while (0)
#define UNLOCK() do { if (dev) SDL_UnlockAudioDevice(dev); } while (0)

static void read_format(DSBuf *b, uint32_t wfx) {
    b->ch = RD16(wfx + 2); b->rate = (int)RD32(wfx + 4); b->bits = RD16(wfx + 14); b->block = RD16(wfx + 12);
    if (!b->block) b->block = b->ch * b->bits / 8;
}
static uint32_t new_buffer(DSBuf *b) { b->refs = 1; b->self = com_new(vt_buf, b); return b->self; }

/* ---------------------------------------------------------------- IDirectSound */
API(DS_QueryInterface, 3) { WR32(A(2), A(0)); THIS_HOST(DSound)->refs++; return DS_OK; }
API(DS_AddRef, 1) { return (uint32_t)++THIS_HOST(DSound)->refs; }
API(DS_Release, 1) { DSound *d = THIS_HOST(DSound); return (uint32_t)(d->refs > 0 ? --d->refs : 0); }
API(DS_Initialize, 2) { audio_open(); return DS_OK; }
API(DS_SetCooperativeLevel, 3) { audio_open(); return DS_OK; }
API(DS_GetCaps, 2) { uint32_t c = A(1), n = RD32(c); memset(GPTR(c + 4), 0, n > 4 ? n - 4 : 0); WR32(c + 4, 0x1F); WR32(c + 12, 100); WR32(c + 16, 48000); return DS_OK; }
API(DS_GetSpeakerConfig, 2) { WR32(A(1), 4); return DS_OK; }   /* DSSPEAKER_STEREO */
API(DS_SetSpeakerConfig, 2) { return DS_OK; }
API(DS_Compact, 1) { return DS_OK; }
API(DS_CreateSoundBuffer, 4) {      /* desc, lplpBuf, outer */
    audio_open();
    uint32_t d = A(1), fl = RD32(d + 4);
    DSBuf *b = calloc(1, sizeof *b);
    b->flags = fl;
    if (fl & DSBCAPS_PRIMARYBUFFER) { b->primary = 1; b->rate = OUT_RATE; b->ch = 2; b->bits = 16; b->block = 4; }
    else {
        b->size = RD32(d + 8);
        if (RD32(d + 16)) read_format(b, RD32(d + 16));
        b->data = galloc(b->size, 1);
        b->data_refs = malloc(sizeof *b->data_refs); *b->data_refs = 1;
        LOCK(); b->next = bufs; bufs = b; UNLOCK();
    }
    WR32(A(2), new_buffer(b));
    TRACE_API("IDirectSound::CreateSoundBuffer(flags 0x%x, %u bytes, %d Hz %d ch %d bit)\n", fl, b->size, b->rate, b->ch, b->bits);
    return DS_OK;
}
API(DS_DuplicateSoundBuffer, 3) {   /* original, lplpDup */
    DSBuf *o = com_host(A(1)), *b = calloc(1, sizeof *b);
    *b = *o; b->playing = 0; b->pos = 0;
    if (b->data_refs) ++*b->data_refs;
    LOCK(); b->next = bufs; bufs = b; UNLOCK();
    WR32(A(2), new_buffer(b));
    return DS_OK;
}

/* ---------------------------------------------------------------- IDirectSoundBuffer */
API(Buf_QueryInterface, 3) { WR32(A(2), 0); return E_NOINTERFACE; }
API(Buf_AddRef, 1) { return (uint32_t)++THIS_HOST(DSBuf)->refs; }
API(Buf_Release, 1) {
    DSBuf *b = THIS_HOST(DSBuf);
    if (--b->refs > 0) return (uint32_t)b->refs;
    LOCK();
    for (DSBuf **p = &bufs; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
    UNLOCK();
    if (b->data_refs && --*b->data_refs == 0) { gfree(b->data); free(b->data_refs); }
    com_free(b->self);
    free(b);
    return 0;
}
API(Buf_GetCaps, 2) { uint32_t c = A(1); WR32(c + 4, THIS_HOST(DSBuf)->flags); WR32(c + 8, THIS_HOST(DSBuf)->size); return DS_OK; }
static uint32_t play_cursor(DSBuf *b) { return b->block ? ((uint32_t)b->pos * (uint32_t)b->block) % (b->size ? b->size : 1) : 0; }
API(Buf_GetCurrentPosition, 3) {
    DSBuf *b = THIS_HOST(DSBuf);
    LOCK(); uint32_t p = play_cursor(b); UNLOCK();
    uint32_t lead = (uint32_t)(b->rate / 50) * (uint32_t)b->block;            /* write cursor ~20 ms ahead */
    if (A(1)) WR32(A(1), p);
    if (A(2)) WR32(A(2), b->size ? (p + lead) % b->size : 0);
    return DS_OK;
}
API(Buf_GetFormat, 4) {             /* wfx, size, written* */
    DSBuf *b = THIS_HOST(DSBuf);
    if (A(1) && A(2) >= 16) {
        WR16(A(1), 1); WR16(A(1) + 2, (uint16_t)b->ch); WR32(A(1) + 4, (uint32_t)b->rate);
        WR32(A(1) + 8, (uint32_t)(b->rate * b->block)); WR16(A(1) + 12, (uint16_t)b->block); WR16(A(1) + 14, (uint16_t)b->bits);
        if (A(2) >= 18) WR16(A(1) + 16, 0);
    }
    if (A(3)) WR32(A(3), 18);
    return DS_OK;
}
API(Buf_GetVolume, 2) { WR32(A(1), (uint32_t)THIS_HOST(DSBuf)->vol); return DS_OK; }
API(Buf_GetPan, 2) { WR32(A(1), (uint32_t)THIS_HOST(DSBuf)->pan); return DS_OK; }
API(Buf_GetFrequency, 2) { DSBuf *b = THIS_HOST(DSBuf); WR32(A(1), b->freq ? b->freq : (uint32_t)b->rate); return DS_OK; }
API(Buf_GetStatus, 2) { DSBuf *b = THIS_HOST(DSBuf); WR32(A(1), b->playing ? (1u | (b->loop ? 4u : 0)) : 0); return DS_OK; }
API(Buf_Lock, 8) {                  /* offset, bytes, p1*, n1*, p2*, n2*, flags */
    DSBuf *b = THIS_HOST(DSBuf);
    if (!b->size) return DSERR_INVALIDPARAM;
    uint32_t off = A(1), n = A(2);
    if (A(7) & DSBLOCK_FROMWRITECURSOR) off = (play_cursor(b) + (uint32_t)(b->rate / 50) * (uint32_t)b->block) % b->size;
    if ((A(7) & DSBLOCK_ENTIREBUFFER) || n > b->size) n = b->size;
    off %= b->size;
    uint32_t n1 = n < b->size - off ? n : b->size - off;
    WR32(A(3), b->data + off); WR32(A(4), n1);
    if (A(5)) WR32(A(5), n1 < n ? b->data : 0);
    if (A(6)) WR32(A(6), n - n1);
    return DS_OK;
}
API(Buf_Unlock, 5) { return DS_OK; }
API(Buf_Play, 4) {                  /* reserved, priority, flags */
    DSBuf *b = THIS_HOST(DSBuf);
    LOCK(); b->playing = 1; b->loop = (A(3) & DSBPLAY_LOOPING) != 0; UNLOCK();
    return DS_OK;
}
API(Buf_Stop, 1) { DSBuf *b = THIS_HOST(DSBuf); LOCK(); b->playing = 0; UNLOCK(); return DS_OK; }
API(Buf_SetCurrentPosition, 2) { DSBuf *b = THIS_HOST(DSBuf); LOCK(); b->pos = b->block ? (double)(A(1) / (uint32_t)b->block) : 0; UNLOCK(); return DS_OK; }
API(Buf_SetFormat, 2) { DSBuf *b = THIS_HOST(DSBuf); if (!b->primary) read_format(b, A(1)); return DS_OK; }
API(Buf_SetVolume, 2) { THIS_HOST(DSBuf)->vol = (int32_t)A(1); return DS_OK; }
API(Buf_SetPan, 2) { THIS_HOST(DSBuf)->pan = (int32_t)A(1); return DS_OK; }
API(Buf_SetFrequency, 2) { THIS_HOST(DSBuf)->freq = A(1); return DS_OK; }
API(Buf_Restore, 1) { return DS_OK; }
API(Buf_Initialize, 3) { return DS_OK; }

#define I(T, n) {#n, w_##T##_##n}
static void vtables(void) {
    if (vt_ds) return;
    static const ComMethod ds[] = {I(DS, QueryInterface), I(DS, AddRef), I(DS, Release), I(DS, CreateSoundBuffer),
        I(DS, GetCaps), I(DS, DuplicateSoundBuffer), I(DS, SetCooperativeLevel), I(DS, Compact), I(DS, GetSpeakerConfig),
        I(DS, SetSpeakerConfig), I(DS, Initialize)};
    static const ComMethod bf[] = {I(Buf, QueryInterface), I(Buf, AddRef), I(Buf, Release), I(Buf, GetCaps),
        I(Buf, GetCurrentPosition), I(Buf, GetFormat), I(Buf, GetVolume), I(Buf, GetPan), I(Buf, GetFrequency),
        I(Buf, GetStatus), I(Buf, Initialize), I(Buf, Lock), I(Buf, Play), I(Buf, SetCurrentPosition), I(Buf, SetFormat),
        I(Buf, SetVolume), I(Buf, SetPan), I(Buf, SetFrequency), I(Buf, Stop), I(Buf, Unlock), I(Buf, Restore)};
    vt_ds = com_vtbl("IDirectSound", ds, sizeof ds / sizeof *ds);
    vt_buf = com_vtbl("IDirectSoundBuffer", bf, sizeof bf / sizeof *bf);
}
uint32_t dsound_create(void) { vtables(); DSound *d = calloc(1, sizeof *d); d->refs = 1; return com_new(vt_ds, d); }
