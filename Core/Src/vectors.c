/* The one place both hop headers are included, so the digest clash is contained.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <string.h>

#include "vectors.h"
#include "hop_vectors.h"
static const char *const local_hop_digest = HOP_VECTORS_DIGEST;
#undef HOP_VECTORS_DIGEST
#include "hop_v1.h"
#include "pair_v4.h"
#include "wire_v4.h"

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
}
