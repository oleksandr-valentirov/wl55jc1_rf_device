/* The device's half of the invitation, which nothing pinned until this file.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#include <stdio.h>
#include <string.h>

#include "pair_init.h"
#include "pair_v4.h"
#include "radio_protocol.h"

static int failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("FAIL %s:%d  ", __func__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

/* The ids the set was generated under. */
#define V_HUB_ID   0x33442211u
#define V_DEV_ID   0x0000002Au
#define V_NET_ID   0x0001u

/* Presence only: mode OPEN reaches no curve operation at all. */
static const uint8_t DEV_PRIV[32] = { 0 };

static pair_init_ctx_t ctx = {
    .dev_priv   = DEV_PRIV,
    .dev_id     = V_DEV_ID,
    .hub_id     = V_HUB_ID,
    .net_id     = V_NET_ID,
    .enrol_mode = RADIO_ENROL_MODE_OPEN,
};

static void reset(void) {
    pair_init_stats_reset();
    pair_init_forget();
}

/* The published frame through the real verifier, at the real length. */
static void test_frame(void) {
    uint8_t hub_static[32];
    uint32_t sf = 0;
    pair_init_rc_t rc;

    reset();
    CHECK(sizeof(PV_FRAME_INIT) == sizeof(radio_pair_init_t),
          "the published frame is %u bytes and the struct is %u",
          (unsigned)sizeof(PV_FRAME_INIT), (unsigned)sizeof(radio_pair_init_t));

    memset(hub_static, 0xAA, sizeof(hub_static));
    rc = pair_init_verify(&ctx, PV_FRAME_INIT, sizeof(PV_FRAME_INIT),
                          0u, PAIR_INIT_SUPERFRAME - 1u, &sf, hub_static);
    CHECK(rc == PAIR_INIT_OK, "published frame refused, rc %d", (int)rc);
    CHECK(sf == PAIR_INIT_SUPERFRAME, "superframe %lu, want %lu",
          (unsigned long)sf, (unsigned long)PAIR_INIT_SUPERFRAME);

    /* The whole point of the frame: it is where the hub's key comes from. */
    CHECK(memcmp(hub_static, PV_HUB_STATIC, sizeof(hub_static)) == 0,
          "the hub's static key was not taken out of the invitation");
}

/* A refused frame must leave nothing behind, or a forgery teaches a hub key. */
static void test_refusal_writes_nothing(void) {
    uint8_t f[sizeof(PV_FRAME_INIT)];
    uint8_t hub_static[32];
    uint32_t sf;
    pair_init_rc_t rc;

    reset();
    memcpy(f, PV_FRAME_INIT, sizeof(f));
    f[8] ^= 0x01u;                       /* another device's invitation */
    memset(hub_static, 0xAA, sizeof(hub_static));
    rc = pair_init_verify(&ctx, f, sizeof(f), 0u, PAIR_INIT_SUPERFRAME - 1u,
                          &sf, hub_static);
    CHECK(rc == PAIR_INIT_NOT_ADDRESSED, "another device's invitation was taken, rc %d",
          (int)rc);
    for (unsigned i = 0; i < sizeof(hub_static); i++)
        CHECK(hub_static[i] == 0xAAu, "a refused invitation still wrote byte %u", i);
}

/* Every refusal, so the acceptance above is not the only path exercised. */
static void test_refusals(void) {
    uint8_t f[sizeof(PV_FRAME_INIT)];
    uint8_t hub_static[32];
    uint32_t sf;
    pair_init_rc_t rc;

    reset();
    memcpy(f, PV_FRAME_INIT, sizeof(f));
    f[1] = RADIO_PAIR_INIT_VERSION + 1u;
    rc = pair_init_verify(&ctx, f, sizeof(f), 0u, PAIR_INIT_SUPERFRAME - 1u,
                          &sf, hub_static);
    CHECK(rc == PAIR_INIT_BAD_FRAME, "a future version was not refused, rc %d", (int)rc);

    reset();
    memcpy(f, PV_FRAME_INIT, sizeof(f));
    f[4] ^= 0x01u;
    rc = pair_init_verify(&ctx, f, sizeof(f), 0u, PAIR_INIT_SUPERFRAME - 1u,
                          &sf, hub_static);
    CHECK(rc == PAIR_INIT_WRONG_NET, "another network's hub was taken, rc %d", (int)rc);

    /* The mode is the device's own; a claim of the stronger one is refused. */
    reset();
    memcpy(f, PV_FRAME_INIT, sizeof(f));
    f[16] = RADIO_ENROL_MODE_SECRET;
    rc = pair_init_verify(&ctx, f, sizeof(f), 0u, PAIR_INIT_SUPERFRAME - 1u,
                          &sf, hub_static);
    CHECK(rc == PAIR_INIT_BAD_MODE, "a mode this device does not hold was taken, rc %d",
          (int)rc);

    /* The reverse, which is the downgrade: a strict device refuses mode OPEN. */
    reset();
    {
        pair_init_ctx_t strict = ctx;
        strict.enrol_mode = RADIO_ENROL_MODE_SECRET;
        rc = pair_init_verify(&strict, PV_FRAME_INIT, sizeof(PV_FRAME_INIT),
                              0u, PAIR_INIT_SUPERFRAME - 1u, &sf, hub_static);
        CHECK(rc == PAIR_INIT_BAD_MODE, "mode OPEN was accepted by a strict device, rc %d",
              (int)rc);
    }

    /* Mode OPEN carries the field and nothing in it, whatever those bytes. */
    reset();
    memcpy(f, PV_FRAME_INIT, sizeof(f));
    f[sizeof(f) - 1u] ^= 0x01u;
    rc = pair_init_verify(&ctx, f, sizeof(f), 0u, PAIR_INIT_SUPERFRAME - 1u,
                          &sf, hub_static);
    CHECK(rc == PAIR_INIT_BAD_MAC, "a non-zero MAC field was accepted, rc %d", (int)rc);

    /* Equal is the recording, not a fresh invitation. */
    reset();
    rc = pair_init_verify(&ctx, PV_FRAME_INIT, sizeof(PV_FRAME_INIT),
                          0u, PAIR_INIT_SUPERFRAME, &sf, hub_static);
    CHECK(rc == PAIR_INIT_REPLAY, "the ceiling's own superframe was accepted, rc %d",
          (int)rc);
}

/* ADR-0024: a device with no hub key must still reach the accept path. */
static void test_no_hub_key_still_listens(void) {
    uint8_t hub_static[32];
    uint32_t sf = 0;
    pair_init_rc_t rc;

    reset();
    /* ctx carries no hub key at all, and never did: there is no field for one. */
    rc = pair_init_verify(&ctx, PV_FRAME_INIT, sizeof(PV_FRAME_INIT),
                          0u, PAIR_INIT_SUPERFRAME - 1u, &sf, hub_static);
    CHECK(rc == PAIR_INIT_OK,
          "a device holding no hub key was refused, against ADR-0024, rc %d",
          (int)rc);
}

int main(void) {
    printf("pair_v4 %s\n\n", PAIR_VECTORS_DIGEST);
    test_frame();
    test_refusal_writes_nothing();
    test_refusals();
    test_no_hub_key_still_listens();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all invitation checks passed\n");
    return 0;
}
