/* This tree's own hop.c, a reference under renamed symbols. ADR-0029
 * radio_devices_docs/radio/hopping.md */
#pragma once

#include <stdint.h>

/* Channel selection from the specification, not from the hub's source.
 * radio_devices_docs/wl55_device/radio/hopping.md */

#define HOPREF_MAX_CHANNELS 64

/** @brief One keyed block over the 16 bytes as given, and it must be able to fail.
 *  radio_devices_docs/radio/hopping.md */
typedef int (*hopref_prf_t)(void *ctx, const uint8_t in[16], uint8_t out[16]);

typedef struct {
    hopref_prf_t prf;
    void     *prf_ctx;    /**< handed back to prf untouched; the key lives there */
    uint8_t   count;
    uint8_t   deck[HOPREF_MAX_CHANNELS];
    uint32_t  cycle;
    uint8_t   valid;
} hopref_ctx_t;

/** @brief Binds a PRF and a channel count; no key reaches this file.
 *  radio_devices_docs/radio/decisions/0030-radio-stack-is-the-link-layer-and-the-session-layer-is-a-separate-consumer.md */
int hopref_init(hopref_ctx_t *ctx, hopref_prf_t prf, void *prf_ctx, uint8_t count);

/** @brief Draws the channel for a superframe; non-zero when the PRF failed.
 *  radio_devices_docs/radio/hopping.md */
int hopref_channel(hopref_ctx_t *ctx, uint32_t superframe, uint8_t *channel);

/** @brief Maps a hop index onto the grid, skipping the reserved join slot. */
uint8_t hopref_to_grid(uint8_t hop_index);
