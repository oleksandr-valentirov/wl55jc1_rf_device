/* The hop sequence as a contract: a disagreement is silence, not an error.
 * radio_devices_docs/wl55_device/radio/hopping.md */
#include <string.h>

#include "hop_reference.h"
#include "radio_phy.h"

int hopref_init(hopref_ctx_t *ctx, hopref_prf_t prf, void *prf_ctx, uint8_t count) {
    if (ctx == NULL || prf == NULL || count < 2u || count > HOPREF_MAX_CHANNELS)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->prf = prf;
    ctx->prf_ctx = prf_ctx;
    ctx->count = count;
    return 0;
}

/* Two blocks shuffle up to 64 channels; the cycle number is big-endian like any input.
 * radio_devices_docs/wl55_device/radio/hopping.md */
static int build_deck(hopref_prf_t prf, void *prf_ctx, uint8_t count, uint32_t cycle,
                      uint8_t *deck) {
    uint8_t block[16];
    uint8_t stream[32];
    uint8_t i;

    memset(block, 0, sizeof(block));
    block[0] = (uint8_t)(cycle >> 24);
    block[1] = (uint8_t)(cycle >> 16);
    block[2] = (uint8_t)(cycle >> 8);
    block[3] = (uint8_t)cycle;

    if (prf(prf_ctx, block, stream) != 0)
        return -1;
    block[15] = 1;
    if (prf(prf_ctx, block, stream + 16) != 0)
        return -1;

    for (i = 0; i < count; i++)
        deck[i] = i;

    for (i = (uint8_t)(count - 1u); i > 0u; i--) {
        uint8_t j = (uint8_t)(stream[i & 31] % (i + 1u));
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    return 0;
}

int hopref_channel(hopref_ctx_t *ctx, uint32_t superframe, uint8_t *channel) {
    uint32_t cycle;

    if (ctx == NULL || channel == NULL || ctx->count == 0u || ctx->prf == NULL)
        return -1;
    cycle = superframe / ctx->count;
    if (!ctx->valid || ctx->cycle != cycle) {
        /* Cleared first: a half-built deck is still a permutation, so a failure hides.
         * radio_devices_docs/wl55_device/radio/hopping.md */
        ctx->valid = 0;
        if (build_deck(ctx->prf, ctx->prf_ctx, ctx->count, cycle, ctx->deck) != 0)
            return -1;
        ctx->cycle = cycle;
        ctx->valid = 1;
    }
    *channel = ctx->deck[superframe % ctx->count];
    return 0;
}

/* The join slot is contract, and the rule has one home: radio_phy.h. */
uint8_t hopref_to_grid(uint8_t hop_index) {
    return (uint8_t)RADIO_HOP_TO_GRID(hop_index);
}
