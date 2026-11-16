/* ole32: COM entry points. CoCreateInstance only logs for now (DirectX objects arrive in step 3). */
#include "host.h"
static void guid_str(uint32_t p, char *out) {
    sprintf(out, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", RD32(p), RD16(p + 4), RD16(p + 6),
            RD8(p + 8), RD8(p + 9), RD8(p + 10), RD8(p + 11), RD8(p + 12), RD8(p + 13), RD8(p + 14), RD8(p + 15));
}
API(CoInitialize, 1) { return 0; }
API(CoUninitialize, 0) { return 0; }
uint32_t ddraw_create(void);
uint32_t dinput_create(void);
uint32_t dsound_create(void);
uint32_t dplay_peer_create(void), dplay_addr_create(void);
API(CoCreateInstance, 5) {      /* clsid, outer, ctx, iid, out** */
    char c[40], i[40]; guid_str(A(0), c); guid_str(A(3), i);
    uint32_t obj = 0;
    if (!strcmp(c, "D7B70EE0-4340-11CF-B063-0020AFC2CD35")) obj = ddraw_create();
    else if (!strcmp(c, "25E609E0-B259-11CF-BFC7-444553540000")) obj = dinput_create();
    else if (!strcmp(c, "47D4D946-62E8-11CF-93BC-444553540000")) obj = dsound_create();
    else if (!strcmp(c, "286F484D-375E-4458-A272-B138E2F80A6A")) obj = dplay_peer_create();      /* DirectPlay8Peer */
    else if (!strcmp(c, "934A9523-A3CA-4BC5-ADA0-D6D95D979421")) obj = dplay_addr_create();      /* DirectPlay8Address */
    if (obj) { hlog("CoCreateInstance(%s) -> 0x%x\n", c, obj); WR32(A(4), obj); return 0; }
    hlog("CoCreateInstance(clsid %s, iid %s) -> REGDB_E_CLASSNOTREG (not implemented yet)\n", c, i);
    if (A(4)) WR32(A(4), 0);
    return 0x80040154u;
}
const HostExport ole32_exports[] = {
#define X(n) {#n, w_##n},
    X(CoInitialize) X(CoUninitialize) X(CoCreateInstance)
#undef X
    {NULL, NULL}};
