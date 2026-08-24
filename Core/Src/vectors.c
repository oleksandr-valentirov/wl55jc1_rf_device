/* The one place both hop headers are included, so the digest clash is contained.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <string.h>

#include "hop.h"
#include "radio_phy.h"
#include "vectors.h"
#include "hop_vectors.h"
static const char *const local_hop_digest = HOP_VECTORS_DIGEST;
#undef HOP_VECTORS_DIGEST
#include "hop_v1.h"
#include "pair_v4.h"
#include "wire_v4.h"

/* The vectors pin one channel count; the grid must still be it. ROADMAP item 83. */
_Static_assert(HOP_VECTORS_COUNT == RADIO_HOP_COUNT,
               "the local hop set describes a deck this grid no longer draws");

/* The deck this board's hop.c draws, against the published one. ROADMAP item 83.
 * radio_devices_docs/wl55_device/radio/hopping.md */
static int8_t hop_deck_kat(void) {
    hop_ctx_t kat;
    uint8_t ch;

    if (hop_init(&kat, HV_HOP_KEY, HOP_VEC_COUNT) != 0)
        return -1;
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++) {
        if (hop_channel(&kat, i, &ch) != 0)
            return -2;
        if (ch != HV_DECK0[i])
            return -3;
    }
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++) {
        if (hop_channel(&kat, HOP_VEC_COUNT + i, &ch) != 0)
            return -4;
        if (ch != HV_DECK1[i])
            return -5;
    }

    /* Cycles past the pinned two, which no host suite can reach: both replay AES. */
    for (unsigned i = 0; i < sizeof(HV_SAMPLE_CH); i++) {
        if (hop_channel(&kat, HV_SAMPLE_SF[i], &ch) != 0)
            return -6;
        if (ch != HV_SAMPLE_CH[i])
            return -7;
    }
    return 0;
}

void vectors_report(vectors_report_t *out) {
    out->pair_v4    = PAIR_VECTORS_DIGEST;
    out->wire_v4    = WIRE_VECTORS_DIGEST;
    out->hop_shared = HOP_VECTORS_DIGEST;
    out->hop_local  = local_hop_digest;

    /* Values, not digests: two digests from one header agree by construction.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    out->hop_local_matches_shared =
        memcmp(vec_hop_key, HV_HOP_KEY, sizeof(HV_HOP_KEY)) == 0 &&
        memcmp(vec_hop_prf_in, HV_PRF_IN, sizeof(HV_PRF_IN)) == 0 &&
        memcmp(vec_hop_prf_out, HV_PRF_OUT, sizeof(HV_PRF_OUT)) == 0 &&
        memcmp(vec_hop_deck_cycle0, HV_DECK0, sizeof(HV_DECK0)) == 0 &&
        memcmp(vec_hop_deck_cycle1, HV_DECK1, sizeof(HV_DECK1)) == 0 &&
        memcmp(vec_aes_fips_out, HV_FIPS_OUT, sizeof(HV_FIPS_OUT)) == 0;

    /* Constants above, the code itself here: the tables agree without it running. */
    out->hop_deck_rc = hop_deck_kat();
}
