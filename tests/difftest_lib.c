/* Shared library used by tools/difftest.py: runs one lifted function on a caller-provided guest arena. */
#include "x86rt.h"
#include <setjmp.h>

void lifted_register_all(void);
void hle_crt_register(void);
extern jmp_buf *x86_fatal_env;
extern const char *x86_fatal_what;
extern uint32_t x86_fatal_addr;

void difftest_init(uint8_t *mem) {
    MEM = mem;
    lifted_register_all();
    hle_crt_register();
}

/* Returns 0 on normal return, 1 if the runtime hit a fatal error (unknown call target, etc.). */
int difftest_run(uint32_t addr, CPU *ctx) {
    jmp_buf env;
    x86_fatal_env = &env;
    if (setjmp(env)) { x86_fatal_env = 0; return 1; }
    x86_call(ctx, addr);
    x86_fatal_env = 0;
    return 0;
}

const char *difftest_fatal_what(void) { return x86_fatal_what; }
uint32_t difftest_fatal_addr(void) { return x86_fatal_addr; }

void difftest_trace(TraceRec *buf, uint32_t cap) { x86_trace_buf = buf; x86_trace_cap = cap; x86_trace_len = 0; }
uint32_t difftest_trace_len(void) { return x86_trace_len; }
