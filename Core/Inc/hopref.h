#pragma once

#include <stdint.h>

/* The hub's hop.c under its own names, so one board draws both decks.
 * radio_devices_docs/radio/hopping.md */

/* The hub's hop_prf_t shape; the two hop.h collide by filename.
 * radio_devices_docs/radio/phy-seam.md */
typedef int (*hopref_prf_fn)(void *ctx, const uint8_t in[16], uint8_t out[16]);

/** @brief Draws the hub's deck and compares it to the published tables.
 *  @return 0 when every slot and sample matched; the failing stage otherwise.
 */
int8_t hopref_deck_kat(hopref_prf_fn prf, void *prf_ctx, uint8_t count,
                       const uint8_t *deck0, const uint8_t *deck1,
                       const uint32_t *sample_sf, const uint8_t *sample_ch,
                       unsigned samples);

/* The other implementation, so hopref.c stays free of this tree's headers. */
typedef int (*hopref_other_fn)(void *ctx, uint32_t sf, uint8_t *ch);

/** @brief Draws whole cycles from both implementations and compares them.
 *  @return 0 when every channel agreed and every cycle was a permutation.
 *  radio_devices_docs/radio/hopping.md */
int8_t hopref_sweep(hopref_prf_fn prf, void *prf_ctx, uint8_t count,
                    hopref_other_fn other, void *other_ctx,
                    uint32_t cycle_start, uint32_t cycles,
                    uint32_t *checked, uint32_t *first_bad_sf);
