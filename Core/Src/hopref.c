/* The hub's Common/src/hop.c under renamed symbols; included, never copied.
 * radio_devices_docs/radio/hopping.md */
#include <stdint.h>

#include "hopref.h"

/* -iquote puts the hub's inc first, so hop.c's own "hop.h" lands there. */
#define hop_ctx_t          hopref_ctx_t
#define hop_prf_t          hopref_prf_t
#define hop_init           hopref_hop_init
#define hop_channel        hopref_hop_channel
#define hop_to_grid        hopref_hop_to_grid

#include OPENHUB_HOP_H
#include OPENHUB_HOP_C

int8_t hopref_deck_kat(hopref_prf_fn prf, void *prf_ctx, uint8_t count,
                       const uint8_t *deck0, const uint8_t *deck1,
                       const uint32_t *sample_sf, const uint8_t *sample_ch,
                       unsigned samples) {
    hopref_ctx_t kat;
    uint8_t ch;

    /* Staged returns, same scheme as hop_deck_kat: a failure names its layer. */
    if (hopref_hop_init(&kat, (hopref_prf_t)prf, prf_ctx, count) != 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (hopref_hop_channel(&kat, i, &ch) != 0)
            return -2;
        if (ch != deck0[i])
            return -3;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (hopref_hop_channel(&kat, (uint32_t)count + i, &ch) != 0)
            return -4;
        if (ch != deck1[i])
            return -5;
    }

    /* Cycles past the pinned two: no host suite reaches these, both replay AES. */
    for (unsigned i = 0; i < samples; i++) {
        if (hopref_hop_channel(&kat, sample_sf[i], &ch) != 0)
            return -6;
        if (ch != sample_ch[i])
            return -7;
    }
    return 0;
}

int8_t hopref_sweep(hopref_prf_fn prf, void *prf_ctx, uint8_t count,
                    hopref_other_fn other, void *other_ctx,
                    uint32_t cycle_start, uint32_t cycles,
                    uint32_t *checked, uint32_t *first_bad_sf) {
    hopref_ctx_t ref;
    uint32_t n = 0;

    *checked = 0;
    *first_bad_sf = 0;
    if (hopref_hop_init(&ref, (hopref_prf_t)prf, prf_ctx, count) != 0)
        return -1;

    for (uint32_t c = 0; c < cycles; c++) {
        uint8_t seen[HOP_MAX_CHANNELS] = {0};
        uint32_t base = (cycle_start + c) * (uint32_t)count;

        for (uint8_t i = 0; i < count; i++) {
            uint8_t a = 0xFF, b = 0xFF;
            uint32_t sf = base + i;

            if (hopref_hop_channel(&ref, sf, &a) != 0)
                { *first_bad_sf = sf; return -2; }
            if (other(other_ctx, sf, &b) != 0)
                { *first_bad_sf = sf; return -3; }
            if (a != b)
                { *first_bad_sf = sf; return -4; }
            if (a >= count || seen[a])   /* the deck is a permutation, or it is not */
                { *first_bad_sf = sf; return -5; }
            seen[a] = 1;
            n++;
        }
    }
    *checked = n;
    return 0;
}
