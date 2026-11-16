/* DirectInput (IDirectInputA + IDirectInputDevice2A), keyboard only: the game reads the mouse via window messages. */
#include "host.h"
#include <SDL.h>

#define DI_OK 0
typedef struct { int refs; } DInput;
typedef struct { int refs; int acquired; } DIDev;
static uint32_t vt_di, vt_dev;

static uint8_t dik_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t az[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
        return az[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint8_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_AC_BACK: case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58; case SDL_SCANCODE_KP_ENTER: return 0x9C;
    case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    default: return 0;
    }
}

API(DI_QueryInterface, 3) { WR32(A(2), A(0)); THIS_HOST(DInput)->refs++; return DI_OK; }
API(DI_AddRef, 1) { return (uint32_t)++THIS_HOST(DInput)->refs; }
API(DI_Release, 1) { DInput *d = THIS_HOST(DInput); return (uint32_t)(d->refs > 0 ? --d->refs : 0); }
API(DI_Initialize, 3) { return DI_OK; }
API(DI_GetDeviceStatus, 2) { return DI_OK; }
API(DI_EnumDevices, 5) { return DI_OK; }
API(DI_CreateDevice, 4) {           /* guid, lplpDevice, outer */
    hlog("IDirectInput::CreateDevice(%08x-...)\n", RD32(A(1)));
    DIDev *d = calloc(1, sizeof *d); d->refs = 1;
    WR32(A(2), com_new(vt_dev, d));
    return DI_OK;
}

API(Dev_QueryInterface, 3) { WR32(A(2), A(0)); THIS_HOST(DIDev)->refs++; return DI_OK; }
API(Dev_AddRef, 1) { return (uint32_t)++THIS_HOST(DIDev)->refs; }
API(Dev_Release, 1) { DIDev *d = THIS_HOST(DIDev); return (uint32_t)(d->refs > 0 ? --d->refs : 0); }
API(Dev_SetDataFormat, 2) { return DI_OK; }
API(Dev_SetCooperativeLevel, 3) { return DI_OK; }
API(Dev_SetProperty, 3) { return DI_OK; }
API(Dev_SetEventNotification, 2) { return DI_OK; }
API(Dev_Acquire, 1) { THIS_HOST(DIDev)->acquired = 1; return DI_OK; }
API(Dev_Unacquire, 1) { THIS_HOST(DIDev)->acquired = 0; return DI_OK; }
API(Dev_Poll, 1) { return DI_OK; }
API(Dev_GetCapabilities, 2) { uint32_t c = A(1), n = RD32(c); memset(GPTR(c + 4), 0, n > 4 ? n - 4 : 0); WR32(c + 4, 1); WR32(c + 8, 3); WR32(c + 20, 128); return DI_OK; }
API(Dev_GetDeviceState, 3) {        /* size, data */
    void host_pump_events(void);
    host_pump_events();
    uint8_t st[256] = {0};
    int n; const uint8_t *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++) if (k[i]) { uint8_t d = dik_of((SDL_Scancode)i); if (d) st[d] = 0x80; }
    int host_pad_start(void);
    if (host_pad_start()) st[0x01] = 0x80;          /* START -> DIK_ESCAPE */
    memcpy(GPTR(A(2)), st, A(1) < 256 ? A(1) : 256);
    return DI_OK;
}
API(Dev_GetDeviceData, 5) { if (A(3)) WR32(A(3), 0); return DI_OK; }   /* no buffered events */

#define M(n) {#n, NULL}
#define I(T, n) {#n, w_##T##_##n}
static void vtables(void) {
    if (vt_di) return;
    static const ComMethod di[] = {I(DI, QueryInterface), I(DI, AddRef), I(DI, Release), I(DI, CreateDevice),
        I(DI, EnumDevices), I(DI, GetDeviceStatus), M(RunControlPanel), I(DI, Initialize)};
    static const ComMethod dv[] = {I(Dev, QueryInterface), I(Dev, AddRef), I(Dev, Release), I(Dev, GetCapabilities),
        M(EnumObjects), M(GetProperty), I(Dev, SetProperty), I(Dev, Acquire), I(Dev, Unacquire), I(Dev, GetDeviceState),
        I(Dev, GetDeviceData), I(Dev, SetDataFormat), I(Dev, SetEventNotification), I(Dev, SetCooperativeLevel),
        M(GetObjectInfo), M(GetDeviceInfo), M(RunControlPanel), M(Initialize),
        /* IDirectInputDevice2A */
        M(CreateEffect), M(EnumEffects), M(GetEffectInfo), M(GetForceFeedbackState), M(SendForceFeedbackCommand),
        M(EnumCreatedEffectObjects), M(Escape), I(Dev, Poll), M(SendDeviceData)};
    vt_di = com_vtbl("IDirectInputA", di, sizeof di / sizeof *di);
    vt_dev = com_vtbl("IDirectInputDevice2A", dv, sizeof dv / sizeof *dv);
}
uint32_t dinput_create(void) { vtables(); DInput *d = calloc(1, sizeof *d); d->refs = 1; return com_new(vt_di, d); }

API(DirectInputCreateA, 4) { WR32(A(2), dinput_create()); return DI_OK; }    /* hinst, version, lplpDI, outer */
const HostExport dinput_exports[] = {{"DirectInputCreateA", w_DirectInputCreateA}, {NULL, NULL}};
