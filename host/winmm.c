/* WINMM: timers and joystick (no joystick yet; on the Vita this is where the buttons can plug in). */
#include "host.h"
uint32_t host_ticks(void);
API(timeGetDevCaps, 2) { WR32(A(0), 1); WR32(A(0) + 4, 1000000); return 0; }
API(timeBeginPeriod, 1) { return 0; }
API(timeEndPeriod, 1) { return 0; }
API(timeGetTime, 0) { return host_ticks(); }
#define JOYERR_UNPLUGGED 167
API(joyGetNumDevs, 0) { return 0; }
API(joyGetDevCapsA, 3) { return JOYERR_UNPLUGGED; }
API(joyGetPos, 2) { return JOYERR_UNPLUGGED; }
API(joyGetPosEx, 2) { return JOYERR_UNPLUGGED; }
const HostExport winmm_exports[] = {
#define X(n) {#n, w_##n},
    X(timeGetDevCaps) X(timeBeginPeriod) X(timeEndPeriod) X(timeGetTime)
    X(joyGetNumDevs) X(joyGetDevCapsA) X(joyGetPos) X(joyGetPosEx)
#undef X
    {NULL, NULL}};
