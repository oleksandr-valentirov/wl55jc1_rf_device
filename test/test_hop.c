/* The shuffle alone: the AES is replayed from the published stream, not computed.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <stdio.h>
#include <string.h>

#include "hop.h"
#include "radio_phy.h"

/* Both headers name their digest HOP_VECTORS_DIGEST; captured and undefined here.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include "hop_vectors.h"
static const char *const local_digest = HOP_VECTORS_DIGEST;
#undef HOP_VECTORS_DIGEST
#include "hop_v1.h"
static const char *const shared_digest = HOP_VECTORS_DIGEST;

static int fails;
static int prf_calls;

static void check(const char *name, int ok) {
    printf("  %-46s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void eq(const char *name, const void *a, const void *b, size_t n) {
    check(name, memcmp(a, b, n) == 0);
}

/* Recovers the block from the input, so a differently built counter gets none.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
int crypto_aes_ecb_block(const uint8_t *key16, const uint8_t in[16], uint8_t out[16]) {
    static const uint8_t zeros[12] = {0};
    uint32_t cycle = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                     ((uint32_t)in[2] << 8) | in[3];
    const uint8_t *stream;

    prf_calls++;
    if (memcmp(key16, HV_HOP_KEY, 16) != 0)
        return -1;
    if (memcmp(in + 4, zeros, sizeof(zeros) - 1u) != 0)
        return -1;                       /* bytes 4..14 must be zero */
    if (cycle == 0u)      stream = HV_STREAM0;
    else if (cycle == 1u) stream = HV_STREAM1;
    else                  return -1;     /* only these two cycles are published */
    memcpy(out, stream + (in[15] ? 16 : 0), 16);
    return 0;
}

int main(void) {
    hop_ctx_t ctx;
    uint8_t ch;

    printf("local %s   shared hop_v1 %s\n\n", local_digest, shared_digest);

    /* A device agreeing only with its own tooling is what the vectors exist to stop.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    eq("key matches hop_v1", vec_hop_key, HV_HOP_KEY, 16);
    eq("prf input matches hop_v1", vec_hop_prf_in, HV_PRF_IN, 16);
    eq("prf output matches hop_v1", vec_hop_prf_out, HV_PRF_OUT, 16);
    eq("fips key matches hop_v1", vec_aes_fips_key, HV_FIPS_KEY, 16);
    eq("fips output matches hop_v1", vec_aes_fips_out, HV_FIPS_OUT, 16);
    eq("cycle 0 deck matches hop_v1", vec_hop_deck_cycle0, HV_DECK0, 28);
    eq("cycle 1 deck matches hop_v1", vec_hop_deck_cycle1, HV_DECK1, 28);
    check("hop_v1 pins the same channel count", HOP_VECTORS_COUNT == HOP_VEC_COUNT);
    /* A fixture the live deck outgrew validates a cycle nothing transmits on.
     * radio_devices_docs/radio/hopping.md */
    check("hop_v1 still describes the live deck",
          HOP_VECTORS_COUNT == RADIO_HOP_COUNT);

    /* The shuffle itself, over replayed AES. */
    check("hop_init", hop_init(&ctx, HV_HOP_KEY, HOP_VEC_COUNT) == 0);
    int deck0 = 1, deck1 = 1;
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++) {
        if (hop_channel(&ctx, i, &ch) != 0 || ch != HV_DECK0[i])
            deck0 = 0;
    }
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++) {
        if (hop_channel(&ctx, HOP_VEC_COUNT + i, &ch) != 0 || ch != HV_DECK1[i])
            deck1 = 0;
    }
    check("cycle 0 deck from the replayed stream", deck0);
    check("cycle 1 deck from the replayed stream", deck1);

    /* Two blocks a cycle: a per-superframe PRF would be correct and cost 56.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    check("the PRF ran twice per cycle, not per superframe", prf_calls == 4);

    /* Cycle 0 is all zeroes and reads the same either endian; cycle 1 separates them.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    check("cycle 1 differs from cycle 0", memcmp(HV_DECK0, HV_DECK1, 28) != 0);

    int samples_ok = 1;
    for (int i = 0; i < 10; i++) {
        uint32_t sf = HV_SAMPLE_SF[i];
        /* Only cycles 0 and 1 have a published stream; the rest need the real PRF.
         * radio_devices_docs/wl55_device/testing/host-tests.md */
        if (sf / HOP_VEC_COUNT > 1u)
            continue;
        if (hop_channel(&ctx, sf, &ch) != 0 || ch != HV_SAMPLE_CH[i])
            samples_ok = 0;
    }
    check("published samples inside the replayed cycles", samples_ok);

    /* Park-and-wait's bound is the permutation, not an assumption about it.
     * radio_devices_docs/radio/joining.md */
    int visits_ok = 1;
    for (uint8_t want = 0; want < HOP_VEC_COUNT; want++) {
        int seen0 = 0, seen1 = 0;
        for (uint32_t sf = 0; sf < HOP_VEC_COUNT; sf++) {
            if (hop_channel(&ctx, sf, &ch) != 0 || ch == want)
                seen0++;
            if (hop_channel(&ctx, sf + HOP_VEC_COUNT, &ch) != 0 || ch == want)
                seen1++;
        }
        if (seen0 != 1 || seen1 != 1)
            visits_ok = 0;
    }
    check("every channel is visited once per cycle", visits_ok);

    /* A counter wrong by one must change channel, or a forgery agrees.
     * radio_devices_docs/radio/joining.md */
    int adjacent_ok = 1, boundary_collides = 0;
    for (uint32_t sf = 0; sf + 1u < 2u * HOP_VEC_COUNT; sf++) {
        uint8_t a, b;
        if (hop_channel(&ctx, sf, &a) != 0 || hop_channel(&ctx, sf + 1u, &b) != 0) {
            adjacent_ok = 0;
            continue;
        }
        if (a != b)
            continue;
        if ((sf + 1u) % HOP_VEC_COUNT == 0u)
            boundary_collides = 1;
        else
            adjacent_ok = 0;
    }
    check("an off-by-one counter changes channel inside a cycle", adjacent_ok);
    printf("  %-46s %s\n", "cycle boundary pair (1 in 28, not a failure)",
           boundary_collides ? "collides" : "differs");

    printf("\n%s\n", fails ? "FAILED" : "all checks passed");
    return fails != 0;
}
