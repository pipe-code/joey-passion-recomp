/* Text fields -> the PS Vita on-screen keyboard.
 *
 * The game draws its own text fields (LAN player name, deck export file name, replay title). They all go through
 * one routine, 0x44e900 (thiscall, `this` = the field), which runs every frame while a field has focus and
 * nowhere else. hle_game.c wraps it and calls host_text_field() first. The field object keeps its text as an
 * std::string at +0x18 (data pointer at +0x1c, length at +0x20; at most 20 characters) and the cursor at +0x28.
 *
 * On the Vita, the first frame of a field opens the system keyboard (sceImeDialog) prefilled with the field's
 * text; when the player confirms, the field is emptied and refilled through the game's own routines
 * (0x44e7d0 delete-at, 0x44e6f0 insert-at), so its layout and limits stay consistent. Triangle reopens it.
 * On Android the same happens with a native text dialog (JoeyActivity.showTextInput); the Keyboard side button or
 * Triangle on a gamepad reopens it.
 * On the desktop the real keyboard works as on Windows; JOEY_TYPE="text" fills the first field (for tests). */
#include "host.h"
#ifdef __vita__
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>
#include <psp2/common_dialog.h>
#endif
#ifdef __ANDROID__
#include <SDL.h>
#include <jni.h>
#endif

#define FIELD_DELETE_AT 0x44e7d0u      /* void __thiscall (int pos) */
#define FIELD_INSERT_AT 0x44e6f0u      /* void __thiscall (char c, int pos), pos < 0 = end; advances the cursor */
#define FIELD_ALLOWED   0x44ec64u      /* byte per char 0x20..0x7c: 0 = rejected (file-name characters) */
#define FIELD_MAX       20

static uint32_t field, field_seen;    /* field object and last frame it was active */
static int offered;                    /* keyboard already offered for this field */
static char pending[64]; static int has_pending;
static int ime_open;

uint32_t host_ticks(void);

static void field_text(uint32_t obj, char *out, size_t n) {
    uint32_t p = RD32(obj + 0x1c), len = RD32(obj + 0x20);
    size_t i = 0;
    for (; p && i < len && i + 1 < n; i++) out[i] = (char)RD8(p + (uint32_t)i);
    out[i] = 0;
}
static void thiscall1(CPU *ctx, uint32_t obj, uint32_t fn, uint32_t a) {
    uint32_t ecx = ctx->ecx; ctx->ecx = obj; guest_call(ctx, fn, 1, a); ctx->ecx = ecx;
}
static void thiscall2(CPU *ctx, uint32_t obj, uint32_t fn, uint32_t a, uint32_t b) {
    uint32_t ecx = ctx->ecx; ctx->ecx = obj; guest_call(ctx, fn, 2, a, b); ctx->ecx = ecx;
}
static void replace_text(CPU *ctx, uint32_t obj, const char *s) {
    for (int guard = 0; RD32(obj + 0x20) > 0 && guard < 256; guard++) thiscall1(ctx, obj, FIELD_DELETE_AT, 0);
    WR32(obj + 0x28, 0); WR32(obj + 0x2c, 0);
    for (const unsigned char *c = (const unsigned char *)s; *c && RD32(obj + 0x20) < FIELD_MAX; c++) {
        if (*c < 0x20) continue;
        if (*c == ' ') { thiscall2(ctx, obj, FIELD_INSERT_AT, '_', 0xFFFFFFFFu); continue; }   /* the game rejects spaces */
        if (*c <= 0x7c && !RD8(FIELD_ALLOWED + (*c - 0x20u))) continue;
        thiscall2(ctx, obj, FIELD_INSERT_AT, (uint32_t)(int8_t)*c, 0xFFFFFFFFu);
    }
    char now[64]; field_text(obj, now, sizeof now);
    hlog("text field: set to \"%s\"\n", now);
}

#ifdef __vita__
static SceWChar16 ime_title[32], ime_init[FIELD_MAX + 1], ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static void ime_open_for(uint32_t obj) {
    char cur[64]; field_text(obj, cur, sizeof cur);
    const char *t = "Joey the Passion";
    for (int i = 0; t[i]; i++) ime_title[i] = (SceWChar16)t[i];
    memset(ime_init, 0, sizeof ime_init); memset(ime_buf, 0, sizeof ime_buf);
    for (int i = 0; cur[i] && i < FIELD_MAX; i++) ime_init[i] = (unsigned char)cur[i];
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_SPANISH | SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title; p.maxTextLength = FIELD_MAX;
    p.initialText = ime_init; p.inputTextBuffer = ime_buf;
    int r = sceImeDialogInit(&p);
    if (r < 0) { hlog("keyboard: sceImeDialogInit failed 0x%08x\n", r); return; }
    ime_open = 1;
    hlog("keyboard: opened (\"%s\")\n", cur);
}
#endif

#ifdef __ANDROID__
/* JoeyActivity.showTextInput(String text, int maxLength) shows a dialog; its result comes back on the UI thread */
static volatile int and_done; static int and_ok; static char and_text[64];
JNIEXPORT void JNICALL Java_io_github_joeyport_JoeyActivity_nativeTextResult(JNIEnv *env, jclass cls, jstring s, jboolean ok) {
    (void)cls;
    int k = 0;
    if (ok && s) {
        const jchar *c = (*env)->GetStringChars(env, s, NULL);
        jsize n = (*env)->GetStringLength(env, s);
        for (jsize i = 0; i < n && k < (int)sizeof and_text - 1; i++) and_text[k++] = c[i] < 0x100 ? (char)c[i] : '?';   /* Latin-1 */
        (*env)->ReleaseStringChars(env, s, c);
    }
    and_text[k] = 0; and_ok = ok;
    __sync_synchronize(); and_done = 1;
}
static void java_static(const char *name, const char *sig, jstring arg, int n) {
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject act = SDL_AndroidGetActivity();
    if (!env || !act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, cls, name, sig);
    if (m) { if (arg) (*env)->CallStaticVoidMethod(env, cls, m, arg, n); else (*env)->CallStaticVoidMethod(env, cls, m); }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls); (*env)->DeleteLocalRef(env, act);
}
static void ime_open_for(uint32_t obj) {
    char cur[64], utf[128]; field_text(obj, cur, sizeof cur);
    int k = 0;
    for (const unsigned char *c = (const unsigned char *)cur; *c && k < (int)sizeof utf - 3; c++) {   /* Latin-1 -> UTF-8 */
        if (*c < 0x80) utf[k++] = (char)*c; else { utf[k++] = (char)(0xC0 | *c >> 6); utf[k++] = (char)(0x80 | (*c & 0x3F)); }
    }
    utf[k] = 0;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (!env) return;
    jstring js = (*env)->NewStringUTF(env, utf);
    and_done = 0; ime_open = 1;
    java_static("showTextInput", "(Ljava/lang/String;I)V", js, FIELD_MAX);
    (*env)->DeleteLocalRef(env, js);
    hlog("keyboard: opened (\"%s\")\n", cur);
}
#endif

/* polled every frame from the message pump: finishes / closes the keyboard */
void host_text_poll(void) {
#ifdef __vita__
    if (!ime_open) return;
    int gone = host_ticks() - field_seen > 500;     /* the game closed the field (e.g. Cancel) */
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult res; memset(&res, 0, sizeof res);
        sceImeDialogGetResult(&res);
        if (res.button == SCE_IME_DIALOG_BUTTON_ENTER && !gone) {
            int n = 0;
            for (int i = 0; ime_buf[i] && n < (int)sizeof pending - 1; i++)
                pending[n++] = ime_buf[i] < 0x100 ? (char)ime_buf[i] : '?';     /* Latin-1 / cp1252 */
            pending[n] = 0; has_pending = 1;
        }
        sceImeDialogTerm(); ime_open = 0;
        hlog("keyboard: closed (%s)\n", res.button == SCE_IME_DIALOG_BUTTON_ENTER ? "enter" : "cancel");
    } else if (gone) { sceImeDialogAbort(); }
#elif defined(__ANDROID__)
    if (!ime_open) return;
    int gone = host_ticks() - field_seen > 500;
    if (and_done) {
        __sync_synchronize();
        if (and_ok && !gone) { snprintf(pending, sizeof pending, "%s", and_text); has_pending = 1; }
        and_done = 0; ime_open = 0;
        hlog("keyboard: closed (%s)\n", and_ok ? "ok" : "cancel");
    } else if (gone) {
        java_static("dismissTextInput", "()V", NULL, 0);     /* reports a cancel */
    }
#endif
}
int host_text_field_active(void) { return field && host_ticks() - field_seen < 500; }
int host_text_ime_open(void) { return ime_open; }
void host_text_reopen(void) { offered = 0; }

/* called by the 0x44e900 wrapper while a field is focused */
void host_text_field(CPU *ctx) {
    uint32_t obj = ctx->ecx, now = host_ticks();
    if (obj != field || now - field_seen > 500) { field = obj; offered = 0; }
    field_seen = now;
    if (has_pending) { has_pending = 0; replace_text(ctx, obj, pending); }
#if defined(__vita__) || defined(__ANDROID__)
    if (!offered && !ime_open) { offered = 1; ime_open_for(obj); }
#else
    if (!offered) {
        offered = 1;
        const unsigned char *t = (const unsigned char *)getenv("JOEY_TYPE");
        if (t) {                         /* UTF-8 -> Latin-1 */
            char l1[64]; int n = 0;
            for (; *t && n < 63; t++) {
                if (*t < 0x80) l1[n++] = (char)*t;
                else if ((*t & 0xE0) == 0xC0 && (t[1] & 0xC0) == 0x80) { unsigned cp = (*t & 0x1Fu) << 6 | (t[1] & 0x3Fu); l1[n++] = cp < 0x100 ? (char)cp : '?'; t++; }
            }
            l1[n] = 0; replace_text(ctx, obj, l1);
        }
    }
#endif
}
