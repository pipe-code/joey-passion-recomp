/* Game-logic overrides (HLE) for Joey the Passion.
 *
 * Power of Chaos cross-game features: the three games (Yugi, Kaiba, Joey) share one card collection in
 * Common\system.dat; CARD_Pack.bin tags each card with a game bitmask (1 Yugi, 2 Kaiba, 4 Joey).
 *  - 0x5bedd0 "owns a card from another game" gates the "all cards" list view and the Options
 *    "complete Power of Chaos card set" choice. Joey alone can never satisfy it, so it always passes here.
 *  - 0x5bea40 returns this game's mask (4). The post-duel reward draw (0x445640) and its pool counter
 *    (0x47da30) use it to pick only Joey cards; for those two callers it returns all three games (7), so
 *    rewards come from the whole Power of Chaos collection. Everything else still sees 4. */
#include "host.h"

static void hle_owns_foreign_card(CPU *ctx) {
    ctx->eax = 1;
    RET_CDECL(ctx);
}

static void hle_game_mask(CPU *ctx) {
    uint32_t ret = RD32(ctx->esp);
    uint32_t game = RD32(0x5eb600u) & 3;           /* 0 Yugi, 1 Kaiba, 2 Joey */
    uint32_t mask = 1u << game;
    if (ret == 0x4456f4u || ret == 0x47da56u) mask = 7;   /* reward draw, reward pool count */
    ctx->ecx = game; ctx->edx = mask;
    ctx->eax = (ctx->eax & 0xFFFF0000u) | mask;
    RET_CDECL(ctx);
}

static void hle_text_field(CPU *ctx);
void hle_game_register(void) {
    x86_register(0x5bedd0u, hle_owns_foreign_card);
    x86_register(0x5bea40u, hle_game_mask);
    x86_register(0x44e900u, hle_text_field);
}

/* 0x44e900: the shared text-field routine (runs only while a field is focused) -> on-screen keyboard */
void f_44e900(CPU *ctx);
void host_text_field(CPU *ctx);
static void hle_text_field(CPU *ctx) { host_text_field(ctx); f_44e900(ctx); }
