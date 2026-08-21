#pragma once

#include <stdint.h>

/* Channel selection from the specification, not from the hub's source.
 * radio_devices_docs/wl55_device/radio/hopping.md */

#define HOP_MAX_CHANNELS 64

typedef struct {
    uint8_t  key[16];
    uint8_t  count;
    uint8_t  deck[HOP_MAX_CHANNELS];
    uint32_t cycle;
    uint8_t  valid;
} hop_ctx_t;

/** @brief Keys the shuffle for a channel count. */
int hop_init(hop_ctx_t *ctx, const uint8_t *key16, uint8_t count);

/** @brief Draws the channel for a superframe; non-zero when the PRF failed.
 *  radio_devices_docs/radio/hopping.md */
int hop_channel(hop_ctx_t *ctx, uint32_t superframe, uint8_t *channel);

/** @brief Maps a hop index onto the grid, skipping the reserved join slot. */
uint8_t hop_to_grid(uint8_t hop_index);

/** @brief Names the convention a heard channel fits, or NULL if none does. */
const char *hop_identify(hop_ctx_t *ctx, uint32_t superframe, uint8_t observed);

/** @brief Models the accelerator's word swap, so it is checked not trusted. */
int hop_swap32_model(const uint8_t *key16, const uint8_t in[16], uint8_t out[16]);
