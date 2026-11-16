/* KERNEL32 emulation. */
#include "host.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#include <strings.h>
#include <ctype.h>

int handle_type(uint32_t h);
int gfree_aligned(uint32_t p);
int galloc_region(uint32_t p, uint32_t *base, uint32_t *size);

#define INVALID_HANDLE 0xFFFFFFFFu
#define FAKE_EXE "C:\\JOEY\\Joey_Pc.exe"
#define FAKE_DIR "C:\\JOEY"

/* Read-only files get a 128 KB read-ahead buffer and a logical position: the game reads its archives
 * through the CRT's 4 KB stdio buffer, and every syscall on the Vita's memory card costs ~0.1-0.3 ms. */
typedef struct { int fd, ro; int64_t pos, buf_off; uint32_t buf_len; uint8_t *buf; } HFile;
#define RA_SIZE (128 * 1024)
static uint32_t std_handles[3];

static void init_std(void) {
    if (std_handles[0]) return;
    for (int i = 0; i < 3; i++) { HFile *f = calloc(1, sizeof *f); f->fd = i; std_handles[i] = handle_new(H_FILE, f); }
}

static uint32_t copy_out(uint32_t buf, uint32_t n, const char *s) {   /* "A"-style string return */
    uint32_t l = (uint32_t)strlen(s);
    if (!buf || n <= l) return l + 1;
    memcpy(GPTR(buf), s, l + 1);
    return l;
}

/* ---------------------------------------------------------------- strings / code pages */
static const uint16_t cp1252_hi[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
static uint16_t a2w(uint8_t c) { return (c >= 0x80 && c < 0xA0) ? cp1252_hi[c - 0x80] : c; }
static uint8_t w2a(uint16_t w, int *bad) {
    if (w < 0x80 || (w >= 0xA0 && w < 0x100)) return (uint8_t)w;
    for (int i = 0; i < 32; i++) if (cp1252_hi[i] == w) return (uint8_t)(0x80 + i);
    *bad = 1; return '?';
}
static uint32_t wlen(uint32_t p) { uint32_t n = 0; while (RD16(p + 2 * n)) n++; return n; }

API(MultiByteToWideChar, 6) {   /* cp, flags, mb, mbn, wbuf, wn */
    uint32_t src = A(2), dst = A(4); int32_t n = (int32_t)A(3), wn = (int32_t)A(5);
    if (A(0) == 932) TRACE_API("MultiByteToWideChar: cp932 treated as cp1252\n");
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (wn == 0) return (uint32_t)n;
    if (wn < n) { last_error = 122; return 0; }
    for (int32_t i = 0; i < n; i++) WR16(dst + 2 * i, a2w(RD8(src + i)));
    return (uint32_t)n;
}
API(WideCharToMultiByte, 8) {   /* cp, flags, wstr, wn, mb, mbn, defchar, useddef* */
    uint32_t src = A(2), dst = A(4), used = A(7); int32_t n = (int32_t)A(3), mn = (int32_t)A(5);
    if (n < 0) n = (int32_t)wlen(src) + 1;
    if (mn == 0) return (uint32_t)n;
    if (mn < n) { last_error = 122; return 0; }
    int bad = 0;
    for (int32_t i = 0; i < n; i++) WR8(dst + i, w2a(RD16(src + 2 * i), &bad));
    if (used) WR32(used, bad);
    return (uint32_t)n;
}
API(GetACP, 0) { return 1252; }
API(GetOEMCP, 0) { return 437; }
API(IsValidCodePage, 1) { return 1; }
API(GetCPInfo, 2) {
    uint32_t p = A(1);
    memset(GPTR(p), 0, 20); WR32(p, 1); WR8(p + 4, '?');
    return 1;
}
static uint16_t ctype1(uint16_t c) {
    uint16_t t = 0;
    if (c < 0x80) {
        if (isupper(c)) t |= 0x1; if (islower(c)) t |= 0x2; if (isdigit(c)) t |= 0x4; if (isspace(c)) t |= 0x8;
        if (ispunct(c)) t |= 0x10; if (iscntrl(c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40;
        if (isxdigit(c)) t |= 0x80; if (isalpha(c)) t |= 0x100;
    } else if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) t = (c < 0xE0 ? 0x1 : 0x2) | 0x100;
    else if (c == 0xA0) t = 0x8 | 0x40; else t = 0x10;
    return t;
}
API(GetStringTypeW, 4) {        /* type, src, n, out */
    int32_t n = (int32_t)A(2); if (n < 0) n = (int32_t)wlen(A(1)) + 1;
    for (int32_t i = 0; i < n; i++) WR16(A(3) + 2 * i, ctype1(RD16(A(1) + 2 * i)));
    return 1;
}
API(GetStringTypeA, 5) {        /* lcid, type, src, n, out */
    int32_t n = (int32_t)A(3); if (n < 0) n = (int32_t)strlen(GSTR(A(2))) + 1;
    for (int32_t i = 0; i < n; i++) WR16(A(4) + 2 * i, ctype1(RD8(A(2) + i)));
    return 1;
}
static uint16_t map_case(uint16_t c, uint32_t flags) {
    if (flags & 0x100) { if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32; }
    if (flags & 0x200) { if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 32; }
    return c;
}
API(LCMapStringW, 6) {          /* lcid, flags, src, n, dst, dn */
    int32_t n = (int32_t)A(3); if (n < 0) n = (int32_t)wlen(A(2)) + 1;
    if (!A(5)) return (uint32_t)n;
    for (int32_t i = 0; i < n && i < (int32_t)A(5); i++) WR16(A(4) + 2 * i, map_case(RD16(A(2) + 2 * i), A(1)));
    return (uint32_t)n;
}
API(LCMapStringA, 6) {
    int32_t n = (int32_t)A(3); if (n < 0) n = (int32_t)strlen(GSTR(A(2))) + 1;
    if (!A(5)) return (uint32_t)n;
    for (int32_t i = 0; i < n && i < (int32_t)A(5); i++) WR8(A(4) + i, (uint8_t)map_case(RD8(A(2) + i), A(1)));
    return (uint32_t)n;
}
static int cmp_n(uint32_t a, int32_t an, uint32_t b, int32_t bn, int wide, int nocase) {
    if (an < 0) an = wide ? (int32_t)wlen(a) : (int32_t)strlen(GSTR(a));
    if (bn < 0) bn = wide ? (int32_t)wlen(b) : (int32_t)strlen(GSTR(b));
    for (int32_t i = 0; i < an && i < bn; i++) {
        uint16_t x = wide ? RD16(a + 2 * i) : RD8(a + i), y = wide ? RD16(b + 2 * i) : RD8(b + i);
        if (nocase) { x = map_case(x, 0x100); y = map_case(y, 0x100); }
        if (x != y) return x < y ? 1 : 3;
    }
    return an == bn ? 2 : (an < bn ? 1 : 3);
}
API(CompareStringW, 6) { return (uint32_t)cmp_n(A(2), (int32_t)A(3), A(4), (int32_t)A(5), 1, A(1) & 1); }
API(CompareStringA, 6) { return (uint32_t)cmp_n(A(2), (int32_t)A(3), A(4), (int32_t)A(5), 0, A(1) & 1); }
API(lstrlenA, 1) { return A(0) ? (uint32_t)strlen(GSTR(A(0))) : 0; }
API(lstrcpyA, 2) { strcpy(GPTR(A(0)), GSTR(A(1))); return A(0); }
API(lstrcpynA, 3) {
    int32_t n = (int32_t)A(2); if (n <= 0) return A(0);
    char *d = GPTR(A(0)); strncpy(d, GSTR(A(1)), n - 1); d[n - 1] = 0; return A(0);
}
API(lstrcatA, 2) { strcat(GPTR(A(0)), GSTR(A(1))); return A(0); }
API(lstrcmpA, 2) { int r = strcmp(GSTR(A(0)), GSTR(A(1))); return (uint32_t)(r < 0 ? -1 : r > 0); }

/* ---------------------------------------------------------------- locale */
static uint32_t lcid = 0x0C0A;   /* Spanish (modern sort); JOEY_LCID overrides */
API(GetUserDefaultLCID, 0) { const char *e = getenv("JOEY_LCID"); return e ? (uint32_t)strtoul(e, 0, 16) : lcid; }
API(IsValidLocale, 2) { return 1; }
API(EnumSystemLocalesA, 2) { return 1; }
static const char *locale_info(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x1004: return "1252";      /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: return "850";       /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0001: return "0c0a";      /* LOCALE_ILANGUAGE */
    case 0x0003: return "ESN";       /* LOCALE_SABBREVLANGNAME */
    case 0x0007: return "ESP";       /* LOCALE_SABBREVCTRYNAME */
    case 0x0005: return "34";        /* LOCALE_ICOUNTRY */
    case 0x1001: return "Spanish";   /* LOCALE_SENGLANGUAGE */
    case 0x1002: return "Spain";     /* LOCALE_SENGCOUNTRY */
    case 0x0059: return "es";        /* LOCALE_SISO639LANGNAME */
    case 0x005A: return "ES";        /* LOCALE_SISO3166CTRYNAME */
    case 0x000E: return ",";         /* LOCALE_SDECIMAL */
    case 0x000F: return ".";         /* LOCALE_STHOUSAND */
    default: return NULL;
    }
}
API(GetLocaleInfoA, 4) {
    const char *s = locale_info(A(1));
    TRACE_API("GetLocaleInfoA(0x%x, 0x%x) -> %s\n", A(0), A(1), s ? s : "(none)");
    if (!s) { last_error = 1004; return 0; }
    uint32_t l = (uint32_t)strlen(s) + 1;
    if (!A(3)) return l;
    if (A(3) < l) { last_error = 122; return 0; }
    memcpy(GPTR(A(2)), s, l); return l;
}
API(GetLocaleInfoW, 4) {
    const char *s = locale_info(A(1));
    TRACE_API("GetLocaleInfoW(0x%x, 0x%x) -> %s\n", A(0), A(1), s ? s : "(none)");
    if (!s) { last_error = 1004; return 0; }
    uint32_t l = (uint32_t)strlen(s) + 1;
    if (!A(3)) return l;
    if (A(3) < l) { last_error = 122; return 0; }
    for (uint32_t i = 0; i < l; i++) WR16(A(2) + 2 * i, (uint8_t)s[i]);
    return l;
}

/* ---------------------------------------------------------------- process / version / env */
API(GetVersion, 0) { return 0x0A280105u; }      /* Windows XP, build 2600 */
API(GetVersionExA, 1) {
    uint32_t p = A(0);
    WR32(p + 4, 5); WR32(p + 8, 1); WR32(p + 12, 2600); WR32(p + 16, 2);
    strcpy(GPTR(p + 20), "Service Pack 2");
    return 1;
}
static uint32_t cmdline, env_a, env_w;
API(GetCommandLineA, 0) { if (!cmdline) cmdline = guest_strdup("\"" FAKE_EXE "\""); return cmdline; }
API(GetEnvironmentStrings, 0) { if (!env_a) env_a = galloc(4, 1); return env_a; }
API(GetEnvironmentStringsW, 0) { if (!env_w) env_w = galloc(8, 1); return env_w; }
API(FreeEnvironmentStringsA, 1) { return 1; }
API(FreeEnvironmentStringsW, 1) { return 1; }
API(SetEnvironmentVariableA, 2) { return 1; }
API(GetStartupInfoA, 1) { memset(GPTR(A(0)), 0, 68); WR32(A(0), 68); return 0; }
API(GetModuleFileNameA, 3) { return copy_out(A(1), A(2), FAKE_EXE) > A(2) ? A(2) : (uint32_t)strlen(FAKE_EXE); }
API(GetCurrentDirectoryA, 2) { return copy_out(A(1), A(0), FAKE_DIR); }
API(GetCurrentProcess, 0) { return 0xFFFFFFFFu; }
API(ExitProcess, 1) { hlog("ExitProcess(%d)\n", A(0)); exit((int)A(0)); }
API(TerminateProcess, 2) { hlog("TerminateProcess(%d)\n", A(1)); exit((int)A(1)); }
API(SetLastError, 1) { last_error = A(0); return 0; }
API(GetLastError, 0) { return last_error; }
API(SetUnhandledExceptionFilter, 1) { return 0; }
API(UnhandledExceptionFilter, 1) { hlog("UnhandledExceptionFilter called\n"); return 1; }
API(IsBadReadPtr, 2) { return 0; }
API(IsBadWritePtr, 2) { return 0; }
API(IsBadCodePtr, 1) { return 0; }
API(SetHandleCount, 1) { return A(0); }

/* modules: fake handles for the DLLs the game asks about */
static const char *modules[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "dinput.dll", "advapi32.dll", "ole32.dll", "winmm.dll", "dsound.dll", "ddraw.dll"};
#define MOD_BASE 0x7FE00000u
static uint32_t module_handle(const char *name) {
    if (!name) return IMAGE_LO;
    for (unsigned i = 0; i < sizeof modules / sizeof *modules; i++) {
        size_t l = strcspn(modules[i], ".");
        if (strncasecmp(name, modules[i], l) == 0 && (!name[l] || name[l] == '.')) return MOD_BASE + 0x10000u * i;
    }
    return 0;
}
API(GetModuleHandleA, 1) { uint32_t h = module_handle(GSTR(A(0))); TRACE_API("GetModuleHandleA(%s) -> 0x%x\n", A(0) ? GSTR(A(0)) : "NULL", h); return h; }
API(LoadLibraryA, 1) { uint32_t h = module_handle(GSTR(A(0))); hlog("LoadLibraryA(%s) -> 0x%x\n", GSTR(A(0)), h); return h; }
API(FreeLibrary, 1) { return 1; }
API(GetProcAddress, 2) {
    uint32_t h = A(0); const char *name = A(1) < 0x10000 ? "#ordinal" : GSTR(A(1));
    const char *dll = (h >= MOD_BASE && (h - MOD_BASE) / 0x10000u < sizeof modules / sizeof *modules) ? modules[(h - MOD_BASE) / 0x10000u] : "?";
    uint32_t r = thunk_for(dll, name);
    hlog("GetProcAddress(%s, %s) -> 0x%x\n", dll, name, r);
    return r;
}

/* ---------------------------------------------------------------- memory */
API(HeapCreate, 3) { return handle_new(H_HEAP, NULL); }
API(HeapDestroy, 1) { handle_close(A(0)); return 1; }
API(HeapAlloc, 3) { return galloc(A(2), (A(1) & 0x8) != 0); }
API(HeapFree, 3) { gfree(A(2)); return 1; }
API(HeapSize, 3) { return gsize(A(2)); }
API(HeapReAlloc, 4) {
    if (A(1) & 0x10) return A(3) <= gsize(A(2)) ? A(2) : 0;        /* HEAP_REALLOC_IN_PLACE_ONLY */
    uint32_t old = gsize(A(2)), p = grealloc(A(2), A(3), 0);
    if (p && (A(1) & 0x8) && A(3) > old) memset(GPTR(p + old), 0, A(3) - old);
    return p;
}
API(GlobalAlloc, 2) { return galloc(A(1), (A(0) & 0x40) != 0); }
API(GlobalFree, 1) { gfree(A(0)); return 0; }
API(GlobalSize, 1) { return gsize(A(0)); }
API(GlobalReAlloc, 3) { return grealloc(A(0), A(1), (A(2) & 0x40) != 0); }
API(LocalFree, 1) { gfree(A(0)); return 0; }
API(LocalLock, 1) { return A(0); }
API(LocalUnlock, 1) { return 0; }
API(VirtualAlloc, 4) {          /* addr, size, type, protect */
    uint32_t addr = A(0), size = (A(1) + 0xFFF) & ~0xFFFu, base, rsz;
    if (addr) {
        if (galloc_region(addr, &base, &rsz) && addr + size <= base + rsz) return addr & ~0xFFFu;
        TRACE_API("VirtualAlloc at fixed 0x%x refused\n", addr);
        return 0;
    }
    uint32_t p = galloc_aligned(size, 0x10000);
    TRACE_API("VirtualAlloc(0x%x, type 0x%x) -> 0x%x\n", size, A(2), p);
    return p;
}
API(VirtualFree, 3) {
    if (A(2) & 0x8000) return gfree_aligned(A(0));                  /* MEM_RELEASE */
    if (A(2) & 0x4000) memset(GPTR(A(0) & ~0xFFFu), 0, (A(1) + 0xFFF) & ~0xFFFu);   /* MEM_DECOMMIT */
    return 1;
}

/* atomics: guest threads run under the GIL, so plain read-modify-write is atomic for the guest */
API(InterlockedIncrement, 1) { uint32_t v = RD32(A(0)) + 1; WR32(A(0), v); return v; }
API(InterlockedDecrement, 1) { uint32_t v = RD32(A(0)) - 1; WR32(A(0), v); return v; }
API(InterlockedExchange, 2) { uint32_t v = RD32(A(0)); WR32(A(0), A(1)); return v; }
API(CloseHandle, 1) {
    int t = handle_type(A(0));
    if (t == H_FILE) { HFile *f = handle_get(A(0), H_FILE); if (f->fd > 2) close(f->fd); free(f->buf); free(f); }
    else if (t != H_FREE && t != H_THREAD) free(handle_get(A(0), 0));   /* thread structs live until exit */
    handle_close(A(0));
    return 1;
}

/* ---------------------------------------------------------------- time */
static struct timeval t_start;
static uint32_t ms_since_start(void) {
    struct timeval t; gettimeofday(&t, NULL);
    if (!t_start.tv_sec) t_start = t;
    return (uint32_t)((t.tv_sec - t_start.tv_sec) * 1000 + (t.tv_usec - t_start.tv_usec) / 1000) + 10000;
}
uint32_t host_ticks(void) { return ms_since_start(); }
API(GetTickCount, 0) { return ms_since_start(); }
static void systemtime(uint32_t p, struct tm *tm, int ms) {
    WR16(p, tm->tm_year + 1900); WR16(p + 2, tm->tm_mon + 1); WR16(p + 4, tm->tm_wday); WR16(p + 6, tm->tm_mday);
    WR16(p + 8, tm->tm_hour); WR16(p + 10, tm->tm_min); WR16(p + 12, tm->tm_sec); WR16(p + 14, ms);
}
static void tm_of(time_t s, struct tm *tm, int local) {
#ifdef _WIN32
    if (local) localtime_s(tm, &s); else gmtime_s(tm, &s);
#else
    if (local) localtime_r(&s, tm); else gmtime_r(&s, tm);
#endif
}
API(GetLocalTime, 1) { struct timeval t; gettimeofday(&t, NULL); struct tm tm; tm_of(t.tv_sec, &tm, 1); systemtime(A(0), &tm, t.tv_usec / 1000); return 0; }
API(GetSystemTime, 1) { struct timeval t; gettimeofday(&t, NULL); struct tm tm; tm_of(t.tv_sec, &tm, 0); systemtime(A(0), &tm, t.tv_usec / 1000); return 0; }
API(GetTimeZoneInformation, 1) { memset(GPTR(A(0)), 0, 172); return 0; }
API(MulDiv, 3) {
    int64_t a = (int32_t)A(0), b = (int32_t)A(1), c = (int32_t)A(2);
    if (!c) return 0xFFFFFFFFu;
    int64_t n = a * b;
    int64_t r = ((n < 0) != (c < 0)) ? (n - c / 2) / c : (n + c / 2) / c;
    return (uint32_t)(int32_t)r;
}

/* ---------------------------------------------------------------- files */
API(GetStdHandle, 1) { init_std(); int32_t n = (int32_t)A(0); return (n >= -12 && n <= -10) ? std_handles[-10 - n] : INVALID_HANDLE; }
API(SetStdHandle, 2) { return 1; }
API(GetFileType, 1) { HFile *f = handle_get(A(0), H_FILE); return f ? (f->fd <= 2 ? 2 : 1) : 0; }
/* negative lookup cache: paths that failed a read-only open (cleared whenever anything is created) */
#define NEG_N 1024
static uint32_t neg_hash[NEG_N]; static int neg_n;
static uint32_t path_hash(const char *p) { uint32_t h = 2166136261u; for (; *p; p++) h = (h ^ (uint8_t)tolower((uint8_t)*p)) * 16777619u; return h | 1; }
static int neg_has(const char *p) { uint32_t h = path_hash(p); for (int i = 0; i < neg_n; i++) if (neg_hash[i] == h) return 1; return 0; }
static void neg_add(const char *p) { if (neg_n < NEG_N) neg_hash[neg_n++] = path_hash(p); }
static void neg_clear(void) { neg_n = 0; }
API(CreateFileA, 7) {           /* name, access, share, sec, disposition, flags, template */
    char path[1100]; host_path(GSTR(A(0)), path, sizeof path);
    uint32_t acc = A(1), disp = A(4);
    int fl = (acc & 0x40000000u) ? ((acc & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;
    case 2: fl |= O_CREAT | O_TRUNC; break;
    case 4: fl |= O_CREAT; break;
    case 5: fl |= O_TRUNC; break;
    }
    double pt = prof_now();
    int ro = fl == O_RDONLY, fd;
    if (ro && neg_has(path)) fd = -1;           /* the game probes loose files that never exist */
    else {
        fd = open(path, fl | O_BINARY, 0644);
        if (!ro) neg_clear();
        else if (fd < 0) neg_add(path);
    }
    prof_acc[PROF_IO] += prof_now() - pt;
    TRACE_API("CreateFileA(%s -> %s, acc 0x%x, disp %d) -> %s\n", GSTR(A(0)), path, acc, disp, fd < 0 ? "FAIL" : "ok");
    if (fd < 0) { last_error = 2; return INVALID_HANDLE; }
    HFile *f = calloc(1, sizeof *f); f->fd = fd; f->ro = ro;
    last_error = 0;
    return handle_new(H_FILE, f);
}
static ssize_t ra_pread(HFile *f, void *dst, size_t n, int64_t off) {
    if (lseek(f->fd, (off_t)off, SEEK_SET) < 0) return -1;
    return read(f->fd, dst, n);
}
static ssize_t ra_read(HFile *f, uint8_t *dst, uint32_t n) {
    uint32_t done = 0;
    while (done < n) {
        int64_t p = f->pos;
        if (f->buf && p >= f->buf_off && p < f->buf_off + f->buf_len) {
            uint32_t k = (uint32_t)(f->buf_off + f->buf_len - p);
            if (k > n - done) k = n - done;
            memcpy(dst + done, f->buf + (p - f->buf_off), k);
            done += k; f->pos += k;
            continue;
        }
        if (n - done >= RA_SIZE / 2) {              /* big read: straight into the destination */
            ssize_t r = ra_pread(f, dst + done, n - done, p);
            if (r < 0) return done ? (ssize_t)done : -1;
            done += (uint32_t)r; f->pos += r;
            break;
        }
        if (!f->buf) f->buf = malloc(RA_SIZE);
        ssize_t r = ra_pread(f, f->buf, RA_SIZE, p);
        if (r <= 0) { f->buf_len = 0; break; }
        f->buf_off = p; f->buf_len = (uint32_t)r;
    }
    return (ssize_t)done;
}
API(ReadFile, 5) {
    HFile *f = handle_get(A(0), H_FILE);
    if (!f) { last_error = 6; return 0; }
    double pt = prof_now();
    ssize_t n;
    if (f->ro) n = ra_read(f, GPTR(A(1)), A(2));
    else n = read(f->fd, GPTR(A(1)), A(2));
    prof_acc[PROF_IO] += prof_now() - pt;
    if (A(3)) WR32(A(3), n < 0 ? 0 : (uint32_t)n);
    return n >= 0;
}
API(WriteFile, 5) {
    HFile *f = handle_get(A(0), H_FILE);
    if (!f) { last_error = 6; return 0; }
    ssize_t n = write(f->fd, GPTR(A(1)), A(2));
    if (A(3)) WR32(A(3), n < 0 ? 0 : (uint32_t)n);
    return n >= 0;
}
API(SetFilePointer, 4) {        /* h, lo, hi*, method */
    HFile *f = handle_get(A(0), H_FILE);
    if (!f) { last_error = 6; return 0xFFFFFFFFu; }
    int64_t off = (int32_t)A(1);
    if (A(2)) off = (int64_t)(((uint64_t)RD32(A(2)) << 32) | A(1));
    double pt = prof_now();
    off_t r;
    if (f->ro) {                    /* logical position only: no syscall */
        struct stat st;
        int64_t base = A(3) == 0 ? 0 : A(3) == 1 ? f->pos : (fstat(f->fd, &st) ? -1 : (int64_t)st.st_size);
        r = (base < 0 || base + off < 0) ? -1 : (off_t)(base + off);
        if (r >= 0) f->pos = r;
    } else r = lseek(f->fd, off, A(3) == 0 ? SEEK_SET : A(3) == 1 ? SEEK_CUR : SEEK_END);
    prof_acc[PROF_IO] += prof_now() - pt;
    if (r < 0) { last_error = 131; return 0xFFFFFFFFu; }
    if (A(2)) WR32(A(2), (uint32_t)((uint64_t)r >> 32));
    return (uint32_t)r;
}
API(GetFileSize, 2) {
    HFile *f = handle_get(A(0), H_FILE); struct stat st;
    if (!f || fstat(f->fd, &st)) return 0xFFFFFFFFu;
    if (A(1)) WR32(A(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}
API(SetEndOfFile, 1) { HFile *f = handle_get(A(0), H_FILE); return f && ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0; }
API(FlushFileBuffers, 1) { return 1; }
API(DeleteFileA, 1) { char p[1100]; host_path(GSTR(A(0)), p, sizeof p); return unlink(p) == 0; }
API(CreateDirectoryA, 2) { char p[1100]; host_path(GSTR(A(0)), p, sizeof p); return mkdir(p, 0755) == 0; }
API(GetTempPathA, 2) { char p[1100]; host_path("TEMP", p, sizeof p); mkdir(p, 0755); return copy_out(A(1), A(0), FAKE_DIR "\\TEMP\\"); }
API(GetTempFileNameA, 4) {
    static uint32_t ctr = 1;
    uint32_t u = A(2) ? A(2) : ctr++;
    char s[512]; snprintf(s, sizeof s, "%s\\%.3s%04X.TMP", GSTR(A(0)), GSTR(A(1)), u & 0xFFFF);
    strcpy(GPTR(A(3)), s);
    return u;
}

typedef struct { char dir[1100], pat[256]; DIR *d; } HFind;
/* case-insensitive Windows-style wildcard match (* and ?) */
static int wild_match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return 1; for (; *s; s++) if (wild_match(p + 1, s)) return 1; return 0; }
        if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))) return 0;
    }
    return !*s;
}
static int find_next(HFind *h, uint32_t data) {
    struct dirent *e;
    while ((e = readdir(h->d))) {
        if (!wild_match(h->pat, e->d_name) && strcmp(h->pat, "*.*")) continue;
        char full[1400]; snprintf(full, sizeof full, "%s/%s", h->dir, e->d_name);
        struct stat st; stat(full, &st);
        memset(GPTR(data), 0, 320);
        WR32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        WR32(data + 28, (uint32_t)((uint64_t)st.st_size >> 32)); WR32(data + 32, (uint32_t)st.st_size);
        snprintf(GPTR(data + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
API(FindFirstFileA, 2) {
    char path[1100]; host_path(GSTR(A(0)), path, sizeof path);
    HFind *h = calloc(1, sizeof *h);
    char *slash = strrchr(path, '/');
    snprintf(h->pat, sizeof h->pat, "%s", slash ? slash + 1 : path);
    if (slash) { *slash = 0; snprintf(h->dir, sizeof h->dir, "%s", path); } else strcpy(h->dir, ".");
    h->d = opendir(h->dir);
    hlog("FindFirstFileA(%s)\n", GSTR(A(0)));
    if (!h->d || !find_next(h, A(1))) { if (h->d) closedir(h->d); free(h); last_error = 2; return INVALID_HANDLE; }
    return handle_new(H_FIND, h);
}
API(FindNextFileA, 2) { HFind *h = handle_get(A(0), H_FIND); if (h && find_next(h, A(1))) return 1; last_error = 18; return 0; }
API(FindClose, 1) { HFind *h = handle_get(A(0), H_FIND); if (h) { closedir(h->d); free(h); handle_close(A(0)); } return 1; }

/* ---------------------------------------------------------------- resources (walk the PE resource directory) */
static uint32_t rsrc_base(void) {
    uint32_t pe = IMAGE_LO + RD32(IMAGE_LO + 0x3c);
    return IMAGE_LO + RD32(pe + 24 + 96 + 2 * 8);       /* DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE] */
}
static int rsrc_name_eq(uint32_t base, uint32_t name_field, uint32_t want) {
    if (want < 0x10000) return !(name_field & 0x80000000u) && (name_field & 0xFFFF) == want;
    const char *w = GSTR(want);
    if (w[0] == '#') return !(name_field & 0x80000000u) && (name_field & 0xFFFF) == (uint32_t)atoi(w + 1);
    if (!(name_field & 0x80000000u)) return 0;
    uint32_t str = base + (name_field & 0x7FFFFFFFu);
    uint16_t len = RD16(str);
    if (strlen(w) != len) return 0;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t c = RD16(str + 2 + 2 * i);
        if (toupper(c < 128 ? c : '?') != toupper((unsigned char)w[i])) return 0;
    }
    return 1;
}
static uint32_t rsrc_find(uint32_t base, uint32_t dir, uint32_t want, int any) {
    uint32_t n = RD16(dir + 12) + RD16(dir + 14);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = dir + 16 + 8 * i;
        if (any || rsrc_name_eq(base, RD32(e), want)) return RD32(e + 4);
    }
    return 0xFFFFFFFFu;
}
API(FindResourceA, 3) {         /* module, name, type -> HRSRC = guest address of the data entry */
    uint32_t base = rsrc_base(), t, nm, lang;
    if ((t = rsrc_find(base, base, A(2), 0)) == 0xFFFFFFFFu || !(t & 0x80000000u) ||
        (nm = rsrc_find(base, base + (t & 0x7FFFFFFFu), A(1), 0)) == 0xFFFFFFFFu || !(nm & 0x80000000u) ||
        (lang = rsrc_find(base, base + (nm & 0x7FFFFFFFu), 0, 1)) == 0xFFFFFFFFu) {
        hlog("FindResourceA(%s, type %s) -> not found\n", A(1) < 0x10000 ? "#id" : GSTR(A(1)), A(2) < 0x10000 ? "#id" : GSTR(A(2)));
        last_error = 1813; return 0;
    }
    return base + lang;
}

const HostExport kernel32_exports[] = {
#define X(n) {#n, w_##n},
    X(MultiByteToWideChar) X(WideCharToMultiByte) X(GetACP) X(GetOEMCP) X(IsValidCodePage) X(GetCPInfo)
    X(GetStringTypeW) X(GetStringTypeA) X(LCMapStringW) X(LCMapStringA) X(CompareStringW) X(CompareStringA)
    X(lstrlenA) X(lstrcpyA) X(lstrcpynA) X(lstrcatA) X(lstrcmpA)
    X(GetUserDefaultLCID) X(IsValidLocale) X(EnumSystemLocalesA) X(GetLocaleInfoA) X(GetLocaleInfoW)
    X(GetVersion) X(GetVersionExA) X(GetCommandLineA) X(GetEnvironmentStrings) X(GetEnvironmentStringsW)
    X(FreeEnvironmentStringsA) X(FreeEnvironmentStringsW) X(SetEnvironmentVariableA) X(GetStartupInfoA)
    X(GetModuleFileNameA) X(GetCurrentDirectoryA) X(GetCurrentProcess) X(ExitProcess)
    X(TerminateProcess) X(SetLastError) X(GetLastError) X(SetUnhandledExceptionFilter) X(UnhandledExceptionFilter)
    X(IsBadReadPtr) X(IsBadWritePtr) X(IsBadCodePtr) X(SetHandleCount)
    X(GetModuleHandleA) X(LoadLibraryA) X(FreeLibrary) X(GetProcAddress)
    X(HeapCreate) X(HeapDestroy) X(HeapAlloc) X(HeapFree) X(HeapSize) X(HeapReAlloc)
    X(GlobalAlloc) X(GlobalFree) X(GlobalSize) X(GlobalReAlloc) X(LocalFree) X(LocalLock) X(LocalUnlock)
    X(VirtualAlloc) X(VirtualFree)
   
    X(InterlockedIncrement) X(InterlockedDecrement)
    X(InterlockedExchange)
    X(CloseHandle)
    X(GetTickCount) X(GetLocalTime) X(GetSystemTime) X(GetTimeZoneInformation) X(MulDiv)
    X(GetStdHandle) X(SetStdHandle) X(GetFileType) X(CreateFileA) X(ReadFile) X(WriteFile) X(SetFilePointer)
    X(GetFileSize) X(SetEndOfFile) X(FlushFileBuffers) X(DeleteFileA) X(CreateDirectoryA) X(GetTempPathA)
    X(GetTempFileNameA) X(FindFirstFileA) X(FindNextFileA) X(FindClose) X(FindResourceA)
#undef X
    {NULL, NULL}};
