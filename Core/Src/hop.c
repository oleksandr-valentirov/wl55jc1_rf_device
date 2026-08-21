/* The hop sequence as a contract: a disagreement is silence, not an error.
 * radio_devices_docs/wl55_device/radio/hopping.md */
#include <string.h>

#include "hop.h"
#include "radio_phy.h"
#include "crypto.h"

/* The grid and the reserved join slot are contract, not local policy. */
#define HOP_GRID_COUNT RADIO_GRID_COUNT
#define HOP_JOIN_SLOT  RADIO_JOIN_SLOT

/* Computable, not tabulated, so an unexpected channel can be named at any superframe.
 * radio_devices_docs/wl55_device/radio/hopping.md */
typedef enum {
    HOP_VARIANT_SPEC = 0,   /* counter big-endian, AES over the bytes */
    HOP_VARIANT_LE_COUNTER, /* counter little-endian */
    HOP_VARIANT_CRYP_WORDS  /* what CRYP_DATATYPE_32B does to a byte buffer */
} hop_variant_t;

static const char *const variant_name[] = {
    "spec", "little-endian cycle counter", "CRYP 32-bit datatype"
};

int hop_init(hop_ctx_t *ctx, const uint8_t *key16, uint8_t count) {
    if (ctx == NULL || key16 == NULL || count < 2u || count > HOP_MAX_CHANNELS)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->key, key16, sizeof(ctx->key));
    ctx->count = count;
    return 0;
}

/* What a little-endian core hands the accelerator when nothing swaps it back.
 * radio_devices_docs/wl55_device/radio/hopping.md */
static void swap32(uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i + 3u < len; i += 4u) {
        uint8_t t = b[i]; b[i] = b[i + 3u]; b[i + 3u] = t;
        t = b[i + 1u]; b[i + 1u] = b[i + 2u]; b[i + 2u] = t;
    }
}

static int prf(const uint8_t *key, const uint8_t in[16], uint8_t out[16],
               hop_variant_t variant) {
    uint8_t tmp[16];

    if (variant != HOP_VARIANT_CRYP_WORDS)
        return crypto_aes_ecb_block(key, in, out);

    memcpy(tmp, in, sizeof(tmp));
    swap32(tmp, sizeof(tmp));
    if (crypto_aes_ecb_block(key, tmp, out) != 0)
        return -1;
    swap32(out, 16u);
    return 0;
}

/* Two blocks shuffle up to 64 channels; the cycle number is big-endian like any input.
 * radio_devices_docs/wl55_device/radio/hopping.md */
static int build_deck(const uint8_t *key, uint8_t count, uint32_t cycle,
                      hop_variant_t variant, uint8_t *deck) {
    uint8_t block[16];
    uint8_t stream[32];
    uint8_t i;

    memset(block, 0, sizeof(block));
    if (variant == HOP_VARIANT_SPEC) {
        block[0] = (uint8_t)(cycle >> 24);
        block[1] = (uint8_t)(cycle >> 16);
        block[2] = (uint8_t)(cycle >> 8);
        block[3] = (uint8_t)cycle;
    } else {
        block[0] = (uint8_t)cycle;
        block[1] = (uint8_t)(cycle >> 8);
        block[2] = (uint8_t)(cycle >> 16);
        block[3] = (uint8_t)(cycle >> 24);
    }

    if (prf(key, block, stream, variant) != 0)
        return -1;
    block[15] = 1;
    if (prf(key, block, stream + 16, variant) != 0)
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

int hop_channel(hop_ctx_t *ctx, uint32_t superframe, uint8_t *channel) {
    uint32_t cycle;

    if (ctx == NULL || channel == NULL || ctx->count == 0u)
        return -1;
    cycle = superframe / ctx->count;
    if (!ctx->valid || ctx->cycle != cycle) {
        /* Cleared first: a half-built deck is still a permutation, so the failure is invisible.
         * radio_devices_docs/wl55_device/radio/hopping.md */
        ctx->valid = 0;
        if (build_deck(ctx->key, ctx->count, cycle, HOP_VARIANT_SPEC, ctx->deck) != 0)
            return -1;
        ctx->cycle = cycle;
        ctx->valid = 1;
    }
    *channel = ctx->deck[superframe % ctx->count];
    return 0;
}

uint8_t hop_to_grid(uint8_t hop_index) {
    return (hop_index < HOP_JOIN_SLOT) ? hop_index : (uint8_t)(hop_index + 1u);
}

const char *hop_identify(hop_ctx_t *ctx, uint32_t superframe, uint8_t observed) {
    uint8_t deck[HOP_MAX_CHANNELS];

    if (ctx == NULL || ctx->count == 0u)
        return NULL;
    for (int v = 0; v < 3; v++) {
        if (build_deck(ctx->key, ctx->count, superframe / ctx->count,
                       (hop_variant_t)v, deck) != 0)
            return NULL;
        if (deck[superframe % ctx->count] == observed)
            return variant_name[v];
    }
    return NULL;
}

/* What a 32-bit datatype should do to one block, computed the long way with swaps.
 * radio_devices_docs/wl55_device/radio/hopping.md */
int hop_swap32_model(const uint8_t *key16, const uint8_t in[16], uint8_t out[16]) {
    return prf(key16, in, out, HOP_VARIANT_CRYP_WORDS);
}
