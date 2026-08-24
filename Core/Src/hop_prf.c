/* The keyed block hop.c draws its deck through, and the wrong one beside it.
 * radio_devices_docs/wl55_device/radio/hopping.md */
#include <string.h>

#include "hop_prf.h"
#include "crypto.h"

int hop_prf_aes(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    return crypto_aes_ecb_block((const uint8_t *)ctx, in, out);
}

/* What a little-endian core hands the accelerator when nothing swaps it back.
 * radio_devices_docs/wl55_device/radio/hopping.md */
static void swap32(uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i + 3u < len; i += 4u) {
        uint8_t t = b[i]; b[i] = b[i + 3u]; b[i + 3u] = t;
        t = b[i + 1u]; b[i + 1u] = b[i + 2u]; b[i + 2u] = t;
    }
}

int hop_prf_swap32(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t tmp[16];

    memcpy(tmp, in, sizeof(tmp));
    swap32(tmp, sizeof(tmp));
    if (crypto_aes_ecb_block((const uint8_t *)ctx, tmp, out) != 0)
        return -1;
    swap32(out, 16u);
    return 0;
}
