/* Minimal COM object model for host-implemented interfaces. */
#include "host.h"

uint32_t com_vtbl(const char *iface, const ComMethod *m, int n) {
    uint32_t v = galloc(4u * n, 1);
    for (int i = 0; i < n; i++) {
        char full[96]; snprintf(full, sizeof full, "%s::%s", iface, m[i].name);
        WR32(v + 4u * i, host_add_thunk(full, m[i].fn));
    }
    return v;
}
uint32_t com_new(uint32_t vtbl, void *host) {
    uint32_t o = galloc(16, 1);
    WR32(o, vtbl);
    WR32(o + 4, handle_new(H_OTHER, host));
    return o;
}
void com_free(uint32_t self) { if (!self) return; handle_close(RD32(self + 4)); gfree(self); }
void *com_host(uint32_t self) { return self ? handle_get(RD32(self + 4), 0) : NULL; }
