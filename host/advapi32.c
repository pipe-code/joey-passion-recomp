/* ADVAPI32: registry persisted in <game_dir>/registry.txt as  KEY|NAME|TYPE|HEXDATA  lines. */
#include "host.h"
#include <strings.h>

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define MAXV 256

typedef struct { char key[256], name[128]; uint32_t type, len; uint8_t data[1024]; } RegVal;
static RegVal vals[MAXV];
static int nvals, loaded;
typedef struct { char path[256]; } RegKey;

static void reg_file(char *out, size_t n) { snprintf(out, n, "%s/registry.txt", game_dir); }
static void load(void) {
    if (loaded) return; loaded = 1;
    char p[1100]; reg_file(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[3000];
    while (nvals < MAXV && fgets(line, sizeof line, f)) {
        RegVal *v = &vals[nvals];
        char *k = strtok(line, "|"), *nm = strtok(NULL, "|"), *t = strtok(NULL, "|"), *hx = strtok(NULL, "|\n");
        if (!k || !nm || !t) continue;
        snprintf(v->key, sizeof v->key, "%s", k); snprintf(v->name, sizeof v->name, "%s", strcmp(nm, "@") ? nm : "");
        v->type = (uint32_t)strtoul(t, 0, 10); v->len = 0;
        for (char *h = hx; h && h[0] && h[1] && v->len < sizeof v->data; h += 2) { unsigned b; sscanf(h, "%2x", &b); v->data[v->len++] = (uint8_t)b; }
        nvals++;
    }
    fclose(f);
}
static void save(void) {
    char p[1100]; reg_file(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < nvals; i++) {
        fprintf(f, "%s|%s|%u|", vals[i].key, vals[i].name[0] ? vals[i].name : "@", vals[i].type);
        for (uint32_t j = 0; j < vals[i].len; j++) fprintf(f, "%02x", vals[i].data[j]);
        fputc('\n', f);
    }
    fclose(f);
}
static RegVal *find(const char *key, const char *name) {
    for (int i = 0; i < nvals; i++) if (!strcasecmp(vals[i].key, key) && !strcasecmp(vals[i].name, name)) return &vals[i];
    return NULL;
}
static const char *root(uint32_t h) {
    switch (h) { case 0x80000000u: return "HKCR"; case 0x80000001u: return "HKCU"; case 0x80000002u: return "HKLM";
                 case 0x80000003u: return "HKU"; default: return NULL; }
}
static void key_path(uint32_t h, const char *sub, char *out, size_t n) {
    const char *r = root(h);
    if (r) snprintf(out, n, "%s", r);
    else { RegKey *k = handle_get(h, H_REGKEY); snprintf(out, n, "%s", k ? k->path : "?"); }
    if (sub && *sub) { size_t l = strlen(out); snprintf(out + l, n - l, "\\%s", sub); }
}

API(RegCreateKeyExA, 9) {       /* hkey, subkey, reserved, class, options, sam, sec, phkResult, disposition* */
    load();
    RegKey *k = calloc(1, sizeof *k);
    key_path(A(0), GSTR(A(1)), k->path, sizeof k->path);
    WR32(A(7), handle_new(H_REGKEY, k));
    if (A(8)) WR32(A(8), 2);    /* REG_OPENED_EXISTING_KEY */
    hlog("RegCreateKeyExA(%s)\n", k->path);
    return ERROR_SUCCESS;
}
API(RegCloseKey, 1) { RegKey *k = handle_get(A(0), H_REGKEY); if (k) { free(k); handle_close(A(0)); } return ERROR_SUCCESS; }
API(RegQueryValueExA, 6) {      /* hkey, name, reserved, type*, data, cbData* */
    load();
    char key[256]; key_path(A(0), NULL, key, sizeof key);
    const char *name = A(1) ? GSTR(A(1)) : "";
    RegVal *v = find(key, name);
    hlog("RegQueryValueExA(%s, %s) -> %s\n", key, name, v ? "found" : "not found");
    if (!v) return ERROR_FILE_NOT_FOUND;
    if (A(3)) WR32(A(3), v->type);
    /* Strings are often stored without their terminating NUL (the game writes PlayerName with strlen bytes);
     * like Windows' ANSI registry API, hand them back terminated. */
    int is_str = v->type == 1 || v->type == 2 || v->type == 7;
    uint32_t len = v->len + (is_str && (!v->len || v->data[v->len - 1]) ? 1 : 0);
    if (A(5)) {
        uint32_t cap = RD32(A(5));
        WR32(A(5), len);
        if (A(4)) {
            if (cap < len) return ERROR_MORE_DATA;
            memcpy(GPTR(A(4)), v->data, v->len);
            if (len > v->len) WR8(A(4) + v->len, 0);
        }
    }
    return ERROR_SUCCESS;
}
API(RegSetValueExA, 6) {        /* hkey, name, reserved, type, data, cb */
    load();
    char key[256]; key_path(A(0), NULL, key, sizeof key);
    const char *name = A(1) ? GSTR(A(1)) : "";
    RegVal *v = find(key, name);
    if (!v) { if (nvals == MAXV) return 1450; v = &vals[nvals++]; snprintf(v->key, sizeof v->key, "%s", key); snprintf(v->name, sizeof v->name, "%s", name); }
    v->type = A(3); v->len = A(5) < sizeof v->data ? A(5) : sizeof v->data;
    memcpy(v->data, GPTR(A(4)), v->len);
    save();
    hlog("RegSetValueExA(%s, %s, type %u, %u bytes)\n", key, name, v->type, v->len);
    return ERROR_SUCCESS;
}
API(GetUserNameA, 2) {
    const char *u = getenv("JOEY_USERNAME") ? getenv("JOEY_USERNAME") : "Player";
    uint32_t need = (uint32_t)strlen(u) + 1, cap = RD32(A(1));
    WR32(A(1), need);
    if (cap < need) { last_error = 122; return 0; }
    strcpy(GPTR(A(0)), u);
    return 1;
}
const HostExport advapi32_exports[] = {
#define X(n) {#n, w_##n},
    X(RegCreateKeyExA) X(RegCloseKey) X(RegQueryValueExA) X(RegSetValueExA) X(GetUserNameA)
#undef X
    {NULL, NULL}};
