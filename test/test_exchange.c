/* pair_v2 from the hub tree, with the transcript rebuilt rather than sliced.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <stdio.h>
#include <string.h>

#include "exchange.h"
#include "pair_v2.h"
#include "wire_v3.h"

#define HUB_ID  0x33442211u
#define DEV_ID  0x0000002Au

static int fails;

static void check(const char *name, int ok) {
    printf("  %-46s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void eq(const char *name, const void *a, const void *b, size_t n) {
    check(name, memcmp(a, b, n) == 0);
}

int main(void) {
    uint8_t salt[EXCHANGE_SALT_LEN];
    uint8_t transcript[EXCHANGE_TRANSCRIPT_LEN];
    exchange_keys_t k;

    printf("pair_v2 %s   wire_v3 %s\n\n", PAIR_VECTORS_DIGEST, WIRE_VECTORS_DIGEST);

    /* The two ids are literals here; wire_v3's salt is what pins them. */
    uint8_t ids[8] = {
        (uint8_t)(HUB_ID >> 24), (uint8_t)(HUB_ID >> 16),
        (uint8_t)(HUB_ID >> 8),  (uint8_t)HUB_ID,
        (uint8_t)(DEV_ID >> 24), (uint8_t)(DEV_ID >> 16),
        (uint8_t)(DEV_ID >> 8),  (uint8_t)DEV_ID,
    };
    eq("ids reproduce wire_v3 salt", ids, V_SALT, sizeof(V_SALT));

    check("salt length matches the published salt",
          EXCHANGE_SALT_LEN == sizeof(PV_SALT));
    check("transcript length matches the published transcript",
          EXCHANGE_TRANSCRIPT_LEN == sizeof(PV_TRANSCRIPT));

    exchange_salt(HUB_ID, DEV_ID, PAIR_REQ_SUPERFRAME, PV_DEV_NONCE, salt);
    eq("salt", salt, PV_SALT, sizeof(PV_SALT));

    exchange_transcript(HUB_ID, DEV_ID, PAIR_REQ_SUPERFRAME, PV_DEV_NONCE,
                        V_HUB_PUB_C, PV_HUB_EPH_PUB, V_DEV_PUB_C, transcript);
    eq("transcript", transcript, PV_TRANSCRIPT, sizeof(PV_TRANSCRIPT));

    /* Currently true and deliberately not relied on: exchange_salt builds its own bytes.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    eq("salt is the transcript prefix", transcript, salt, sizeof(salt));

    exchange_derive(PV_Z, PV_Z + EXCHANGE_Z_TERM_LEN, salt, transcript, &k);
    eq("session key", k.session, PV_KEY_SESSION, sizeof(PV_KEY_SESSION));
    eq("confirm key hub", k.confirm_key_hub, PV_CONFIRM_KEY_HUB, 32);
    eq("confirm key dev", k.confirm_key_dev, PV_CONFIRM_KEY_DEV, 32);
    eq("confirm hub", k.confirm_hub, PV_CONFIRM_HUB, 16);
    eq("confirm dev", k.confirm_dev, PV_CONFIRM_DEV, 16);

    /* The device's freshness must reach the KDF, or every value above still passes.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    uint8_t salt_bad[EXCHANGE_SALT_LEN];
    uint8_t nonce_bad[EXCHANGE_NONCE_LEN];
    exchange_keys_t k2;
    memcpy(nonce_bad, PV_DEV_NONCE, sizeof(nonce_bad));
    nonce_bad[7] ^= 0x01u;
    exchange_salt(HUB_ID, DEV_ID, PAIR_REQ_SUPERFRAME, nonce_bad, salt_bad);
    exchange_derive(PV_Z, PV_Z + EXCHANGE_Z_TERM_LEN, salt_bad, transcript, &k2);
    check("one nonce bit changes the session key",
          memcmp(k2.session, PV_KEY_SESSION, sizeof(PV_KEY_SESSION)) != 0);

    exchange_salt(HUB_ID, DEV_ID, PAIR_REQ_SUPERFRAME + 1u, PV_DEV_NONCE, salt_bad);
    exchange_derive(PV_Z, PV_Z + EXCHANGE_Z_TERM_LEN, salt_bad, transcript, &k2);
    check("one superframe changes the session key",
          memcmp(k2.session, PV_KEY_SESSION, sizeof(PV_KEY_SESSION)) != 0);

    /* Z1 authenticates, Z2 is fresh: swapping them is a whole-key error.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    exchange_derive(PV_Z + EXCHANGE_Z_TERM_LEN, PV_Z, salt, transcript, &k2);
    check("swapping the Z terms changes the session key",
          memcmp(k2.session, PV_KEY_SESSION, sizeof(PV_KEY_SESSION)) != 0);

    /* Named for what it detects. It says nothing about a replayed response. */
    check("eph-is-static detects the static key reused",
          exchange_eph_is_static(V_HUB_PUB_C, V_HUB_PUB_C) == 1);
    check("eph-is-static passes a real ephemeral",
          exchange_eph_is_static(PV_HUB_EPH_PUB, V_HUB_PUB_C) == 0);

    uint8_t c[EXCHANGE_CONFIRM_LEN];
    memcpy(c, PV_CONFIRM_HUB, sizeof(c));
    check("confirm compare accepts equal", exchange_confirm_equal(c, PV_CONFIRM_HUB) == 1);
    c[EXCHANGE_CONFIRM_LEN - 1] ^= 0x01u;
    check("confirm compare rejects one flipped bit",
          exchange_confirm_equal(c, PV_CONFIRM_HUB) == 0);

    printf("\n%s\n", fails ? "FAILED" : "all checks passed");
    return fails != 0;
}
