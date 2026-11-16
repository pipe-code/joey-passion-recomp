/* Host entry point: maps the user's Joey_Pc.exe into guest memory and runs the recompiled code. */
#include "host.h"
#include "imports.h"
#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
/* lifted code nests one C frame per guest call: give the main thread a big stack */
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;
int _newlib_heap_size_user = 64 * 1024 * 1024;
#define DEFAULT_GAME_DIR "ux0:data/joey"
#define GUEST_SIZE (112u * 1024 * 1024)
#elif defined(_WIN32)
#include <windef.h>                      /* not windows.h: winbase.h defines its own GPTR */
#include <memoryapi.h>
#include <errhandlingapi.h>
#include <SDL.h>                         /* SDL_main entry point */
#define DEFAULT_GAME_DIR "game"
#define GUEST_SIZE 0x20000000u          /* 512 MB reserved (committed lazily by Windows) */
#elif defined(__ANDROID__)
#include <sys/mman.h>
#include <SDL.h>                         /* SDL_main entry point, called by SDLActivity */
#define DEFAULT_GAME_DIR "game"
#define GUEST_SIZE 0x20000000u          /* 512 MB reserved (lazily committed) */
#else
#include <sys/mman.h>
#define DEFAULT_GAME_DIR "game"
#define GUEST_SIZE 0x20000000u          /* 512 MB reserved (lazily committed) */
#endif
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>

void lifted_register_all(void);
void hle_crt_register(void);
void hle_game_register(void);

int host_verbose;
char game_dir[1024] = DEFAULT_GAME_DIR;
uint32_t guest_space = GUEST_SIZE;
uint32_t last_error;
CPU main_cpu;

#ifdef __vita__
static FILE *hlog_file;
static void fatal_to_log(const char *what, uint32_t addr) {
    hlog("x86rt fatal: %s at 0x%08x\n", what, addr);
    if (hlog_file) fflush(hlog_file);
}
#endif
void hlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
#ifdef __vita__
    /* no console on the Vita: log next to the game data */
    FILE *log = hlog_file;
    if (!log) {
        char p[1100]; snprintf(p, sizeof p, "%s/log.txt", game_dir); log = hlog_file = fopen(p, "w");
        extern void (*x86_fatal_hook)(const char *, uint32_t); x86_fatal_hook = fatal_to_log;
    }
    /* buffered: a flush per line on ux0 costs milliseconds; flush at most once a second */
    if (log) {
        static uint32_t last;
        vfprintf(log, fmt, ap);
        uint32_t host_ticks(void); uint32_t now = host_ticks();
        if (now - last >= 1000) { fflush(log); last = now; }
    }
#else
    vfprintf(stderr, fmt, ap);
#endif
    va_end(ap);
}

/* ------------------------------------------------------------------ handles */
#define MAXH 4096
static struct { int type; void *obj; } htab[MAXH];

uint32_t handle_new(int type, void *obj) {
    for (int i = 1; i < MAXH; i++)
        if (htab[i].type == H_FREE) { htab[i].type = type; htab[i].obj = obj; return 0x1000u + 4u * i; }
    hlog("handle table full\n"); exit(1);
}
void *handle_get(uint32_t h, int type) {
    uint32_t i = (h - 0x1000u) / 4;
    if (h < 0x1000u || i >= MAXH || (h & 3) || htab[i].type == H_FREE) return NULL;
    if (type && htab[i].type != type) return NULL;
    return htab[i].obj;
}
int handle_type(uint32_t h) {
    uint32_t i = (h - 0x1000u) / 4;
    return (h < 0x1000u || i >= MAXH) ? H_FREE : htab[i].type;
}
void handle_close(uint32_t h) {
    uint32_t i = (h - 0x1000u) / 4;
    if (h >= 0x1000u && i < MAXH) { htab[i].type = H_FREE; htab[i].obj = NULL; }
}

/* ------------------------------------------------------------------ paths */
/* The game believes it is installed in C:\JOEY. Any absolute path is reduced to its part below the
 * install dir (or taken as-is relative to it), then each component is matched case-insensitively. */
int host_path(const char *gpath, char *out, size_t outsz) {
    char tmp[1024]; size_t n = 0;
    const char *p = gpath;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    if (strncasecmp(p, "JOEY", 4) == 0 && (p[4] == '\\' || p[4] == '/' || !p[4])) { p += 4; while (*p == '\\' || *p == '/') p++; }
    for (; *p && n < sizeof tmp - 1; p++) tmp[n++] = (*p == '\\') ? '/' : *p;
    tmp[n] = 0;
    snprintf(out, outsz, "%s", game_dir);
    char *save, *comp = strtok_r(tmp, "/", &save);
    while (comp) {
        if (strcmp(comp, ".") != 0) {
            char found[256] = "";
            DIR *d = opendir(out);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) if (strcasecmp(e->d_name, comp) == 0) { snprintf(found, sizeof found, "%s", e->d_name); break; }
                closedir(d);
            }
            size_t l = strlen(out);
            snprintf(out + l, outsz - l, "/%s", found[0] ? found : comp);
        }
        comp = strtok_r(NULL, "/", &save);
    }
    return 1;
}

/* ------------------------------------------------------------------ host -> guest calls */
#define GUEST_RET_MAGIC 0x7FFFFFF0u
uint32_t guest_call(CPU *ctx, uint32_t fn, int nargs, ...) {
    CPU save = *ctx;
    uint32_t args[16]; va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    ctx->esp -= 64;                                  /* red zone between host-visible frame and callback */
    for (int i = nargs - 1; i >= 0; i--) { ctx->esp -= 4; WR32(ctx->esp, args[i]); }
    ctx->esp -= 4; WR32(ctx->esp, GUEST_RET_MAGIC);
    x86_call(ctx, fn);
    uint32_t r = ctx->eax;
    uint32_t keep_fcw = ctx->fcw;
    *ctx = save;
    ctx->fcw = keep_fcw;
    return r;
}

/* ------------------------------------------------------------------ imports / thunks */
static const struct { const char *dll; const HostExport *tab; } DLLS[] = {
    {"KERNEL32.dll", kernel32_exports}, {"KERNEL32.dll", threads_exports}, {"USER32.dll", user32_exports}, {"USER32.dll", user32_gdi_exports},
    {"GDI32.dll", gdi32_exports},
    {"ADVAPI32.dll", advapi32_exports}, {"ole32.dll", ole32_exports}, {"OLEAUT32.dll", oleaut32_exports},
    {"WINMM.dll", winmm_exports}, {"MSACM32.dll", msacm32_exports}, {"dinput.dll", dinput_exports},
};
#define NDLLS (int)(sizeof DLLS / sizeof DLLS[0])

#define MAXTHUNK 1024
static struct { uint32_t addr; char name[64]; } thunks[MAXTHUNK];
static int nthunks;

const char *host_thunk_name(uint32_t a) {
    for (int i = 0; i < nthunks; i++) if (thunks[i].addr == a) return thunks[i].name;
    return "?";
}

static void unimplemented(CPU *ctx) {
    hlog("\n*** unimplemented %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x) from ret 0x%08x\n",
         host_thunk_name(x86_current_target), A(0), A(1), A(2), A(3), A(4), A(5), RD32(ctx->esp));
    exit(3);
}

static x86_fn find_export(const char *dll, const char *name) {
    for (int d = 0; d < NDLLS; d++) {
        size_t l = strcspn(DLLS[d].dll, ".");
        if (strncasecmp(dll, DLLS[d].dll, l) != 0 || (dll[l] && dll[l] != '.')) continue;
        for (const HostExport *e = DLLS[d].tab; e->name; e++)
            if (strcmp(e->name, name) == 0) return e->fn;
    }
    return NULL;
}

/* Register a host function under a fresh fake guest address; `full` is used in logs. */
uint32_t host_add_thunk(const char *full, x86_fn fn) {
    if (nthunks == MAXTHUNK) { hlog("too many thunks\n"); exit(1); }
    uint32_t a = THUNK_BASE + 16u * nthunks;
    snprintf(thunks[nthunks].name, sizeof thunks[0].name, "%s", full);
    thunks[nthunks++].addr = a;
    x86_register(a, fn ? fn : unimplemented);
    return a;
}
static uint32_t add_thunk(const char *dll, const char *name, x86_fn fn) {
    char full[64]; snprintf(full, sizeof full, "%s!%s", dll, name);
    return host_add_thunk(full, fn);
}

uint32_t thunk_for(const char *dll, const char *name) {
    x86_fn fn = find_export(dll, name);
    if (!fn) return 0;
    char full[64]; snprintf(full, sizeof full, "%s!%s", dll, name);
    for (int i = 0; i < nthunks; i++) if (strcasecmp(thunks[i].name, full) == 0) return thunks[i].addr;
    return add_thunk(dll, name, fn);
}

static void patch_imports(void) {
    int missing = 0;
    for (int i = 0; i < NUM_IMPORTS; i++) {
        x86_fn fn = find_export(IMPORTS[i].dll, IMPORTS[i].name);
        missing += !fn;
        WR32(IMPORTS[i].iat, add_thunk(IMPORTS[i].dll, IMPORTS[i].name, fn));
    }
    hlog("imports: %d total, %d not implemented yet\n", NUM_IMPORTS, missing);
}

uint32_t guest_strdup(const char *s) {
    uint32_t p = galloc((uint32_t)strlen(s) + 1, 0);
    strcpy(GPTR(p), s);
    return p;
}

/* ------------------------------------------------------------------ PE loading */
static uint32_t load_exe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { hlog("cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len); fread(b, 1, len, f); fclose(f);
    uint32_t pe; memcpy(&pe, b + 0x3c, 4);
    uint16_t nsec, optsz; memcpy(&nsec, b + pe + 6, 2); memcpy(&optsz, b + pe + 20, 2);
    uint32_t opt = pe + 24, base, entry, hdrsz;
    memcpy(&entry, b + opt + 16, 4); memcpy(&base, b + opt + 28, 4); memcpy(&hdrsz, b + opt + 60, 4);
    if (base != IMAGE_LO) { hlog("unexpected image base 0x%x\n", base); exit(1); }
    memcpy(GPTR(base), b, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint8_t *s = b + opt + optsz + 40 * i;
        uint32_t vsz, va, rawsz, raw;
        memcpy(&vsz, s + 8, 4); memcpy(&va, s + 12, 4); memcpy(&rawsz, s + 16, 4); memcpy(&raw, s + 20, 4);
        memcpy(GPTR(base + va), b + raw, rawsz < vsz ? rawsz : vsz);
    }
    free(b);
    return base + entry;
}

#ifdef __APPLE__
#include <SDL.h>
#include <sys/stat.h>
/* Running as Joey.app: the game files ship read-only in Contents/Resources/game. The game writes its save
 * (registry.txt, deck.ydc, replays, the Common\system.dat collection) into its own folder, so play from
 * ~/Library/Application Support/Joey the Passion: big files are symlinked back into the bundle (refreshed on
 * every launch, so moving the app is fine) and the starting save is copied only if there is none yet.
 * Logs go to log.txt there, since a Finder launch has no console. Returns 1 if set up. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"); if (!in) return 0;
    FILE *out = fopen(dst, "wb"); if (!out) { fclose(in); return 0; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out); return 1;
}
static void copy_tree_if_missing(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { if (access(dst, F_OK) != 0) copy_file(src, dst); return; }
    mkdir(dst, 0755);
    DIR *d = opendir(src); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char s2[2048], d2[2048];
        snprintf(s2, sizeof s2, "%s/%s", src, e->d_name); snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
        copy_tree_if_missing(s2, d2);
    }
    closedir(d);
}
static int setup_app_bundle(void) {
    char *base = SDL_GetBasePath();             /* .../Joey.app/Contents/Resources/ for a bundle */
    if (!base || !strstr(base, ".app/Contents/Resources")) { SDL_free(base); return 0; }
    char res[1024], home[1024];
    snprintf(res, sizeof res, "%sgame", base); SDL_free(base);
    const char *h = getenv("HOME"); if (!h) return 0;
    snprintf(home, sizeof home, "%s/Library/Application Support/Joey the Passion", h);
    mkdir(home, 0755);
    static const char *linked[] = {"Joey_Pc.exe", "Data.dat", "Voice.dat", "Voice"};
    for (size_t i = 0; i < sizeof linked / sizeof *linked; i++) {
        char s2[2048], d2[2048];
        snprintf(s2, sizeof s2, "%s/%s", res, linked[i]); snprintf(d2, sizeof d2, "%s/%s", home, linked[i]);
        unlink(d2); symlink(s2, d2);
    }
    char seed[1100]; snprintf(seed, sizeof seed, "%s/save", res);
    copy_tree_if_missing(seed, home);             /* Region.dat, registry.txt, deck.ydc, Common folder */
    snprintf(game_dir, sizeof game_dir, "%s", home);
    char log[1100]; snprintf(log, sizeof log, "%s/log.txt", home);
    freopen(log, "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    return 1;
}
#endif

/* ------------------------------------------------------------------ main */
#ifdef __ANDROID__
/* SDLActivity runs SDL_main on a Java thread with a small stack; lifted code nests one C frame per guest call */
static int android_argc; static char **android_argv;
static int android_game_thread(void *p) { (void)p; return main(android_argc, android_argv); }
#endif
int main(int argc, char **argv) {
#ifdef __ANDROID__
    static int on_game_thread;
    if (!on_game_thread) {
        on_game_thread = 1; android_argc = argc; android_argv = argv;
        SDL_Thread *t = SDL_CreateThreadWithStackSize(android_game_thread, "game", 64u << 20, NULL);
        int r = 1; if (t) SDL_WaitThread(t, &r);
        exit(r);            /* a fresh process for the next launch: the guest state lives in globals */
    }
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) host_verbose = 1;
        else if (strncmp(argv[i], "-psn_", 5)) snprintf(game_dir, sizeof game_dir, "%s", argv[i]);   /* Finder adds -psn_* */
    }
#ifdef __APPLE__
    if (argc < 2 || !strncmp(argv[1], "-psn_", 5)) setup_app_bundle();
#endif
#ifdef _WIN32
    /* Joey.exe next to a "game" folder with the game files and the save; no console: log to game\log.txt */
    if (argc < 2) {
        char *base = SDL_GetBasePath();
        if (base) { snprintf(game_dir, sizeof game_dir, "%sgame", base); SDL_free(base); }
        char log[1100]; snprintf(log, sizeof log, "%s/log.txt", game_dir);
        freopen(log, "w", stderr);
        setvbuf(stderr, NULL, _IOLBF, 0);
    }
#endif
#ifdef __ANDROID__
    /* JoeyActivity passes the game folder (app-specific external storage); no console: log to log.txt there */
    if (argc < 2) {
        const char *ext = SDL_AndroidGetExternalStoragePath();
        if (ext) snprintf(game_dir, sizeof game_dir, "%s/game", ext);
    }
    { char log[1100]; snprintf(log, sizeof log, "%s/log.txt", game_dir);
      freopen(log, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0); }
#endif
    void host_threads_init(void);
    host_threads_init();
#ifdef __vita__
    /* default app clocks are 333 MHz ARM; the recompiled game needs everything we can get */
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    /* Generous guest memory, but leave headroom: every guest thread is a real Vita thread whose stack
     * (and SDL/GXM/audio buffers) come out of the same pool -- taking it all made CreateThread fail. */
    SceKernelFreeMemorySizeInfo fm = {sizeof fm};
    sceKernelGetFreeMemorySize(&fm);
    uint32_t free_mb = (uint32_t)fm.size_user >> 20, want = free_mb > 64 + 112 ? free_mb - 64 : 112;
    if (want > 192) want = 192;
    hlog("memory: %u MB free (user), taking %u MB for the guest\n", free_mb, want);
    SceUID blk = -1;
    for (uint32_t mb = want & ~15u; mb >= GUEST_SIZE >> 20; mb -= 16) {
        blk = sceKernelAllocMemBlock("joey_guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mb << 20, NULL);
        if (blk >= 0) { guest_space = mb << 20; break; }
    }
    if (blk < 0) { hlog("sceKernelAllocMemBlock(%u MB) failed: 0x%08x\n", guest_space >> 20, blk); return 1; }
    void *base; sceKernelGetMemBlockBase(blk, &base);
    MEM = base;
    memset(MEM, 0, guest_space);
#elif defined(_WIN32)
    MEM = VirtualAlloc(NULL, guest_space, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!MEM) { hlog("VirtualAlloc(%u MB) failed: %lu\n", guest_space >> 20, GetLastError()); return 1; }
#else
    MEM = mmap(NULL, guest_space, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (MEM == MAP_FAILED) { perror("mmap"); return 1; }
#endif
    hlog("guest memory: %u MB at %p, game dir %s\n", guest_space >> 20, (void *)MEM, game_dir);

    char exe[1100]; snprintf(exe, sizeof exe, "%s/Joey_Pc.exe", game_dir);
    uint32_t entry = load_exe(exe);
    galloc_init(HEAP_LO, HEAP_HI);
    lifted_register_all();
    void host_prof_start(void); host_prof_start();
    hle_crt_register();
    hle_game_register();
    patch_imports();

    /* PEB / TEB of the main thread */
    WR32(PEB_ADDR + 0x08, IMAGE_LO);
    WR32(TEB_ADDR + 0x00, 0xFFFFFFFFu);      /* SEH chain end */
    WR32(TEB_ADDR + 0x04, STACK_HI);
    WR32(TEB_ADDR + 0x08, STACK_LO);
    WR32(TEB_ADDR + 0x18, TEB_ADDR);
    WR32(TEB_ADDR + 0x20, 1);                /* process id */
    WR32(TEB_ADDR + 0x24, 1);                /* thread id */
    WR32(TEB_ADDR + 0x30, PEB_ADDR);

    main_cpu.esp = STACK_HI - 16;
    main_cpu.fs_base = TEB_ADDR;
    main_cpu.fcw = 0x27F;
    main_cpu.esp -= 4; WR32(main_cpu.esp, GUEST_RET_MAGIC);
    hlog("starting at entry 0x%08x\n", entry);
    x86_call(&main_cpu, entry);
    hlog("entry returned eax=0x%x\n", main_cpu.eax);
    return (int)main_cpu.eax;
}
