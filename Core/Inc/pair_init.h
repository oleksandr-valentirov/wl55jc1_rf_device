#pragma once

#include <stdint.h>

#include "radio_protocol.h"

/* PAIR_INIT is the hub's addressed invitation to pair, and the only frame in
 * pair_v3 that pair_v2 did not have. It is authenticated with a key derived
 * from the two static keys, so a device that has been provisioned with the
 * hub's public key can tell an invitation from a bystander's frame - which the
 * cleartext join beacon it replaces could not. */

typedef enum {
    PAIR_INIT_OK = 0,
    PAIR_INIT_BAD_FRAME,      /* type, version or length */
    PAIR_INIT_WRONG_NET,      /* net_id or hub_id is another network's */
    PAIR_INIT_NOT_ADDRESSED,  /* dev_id names a different device */
    PAIR_INIT_NO_HUB_KEY,     /* Z1 needs the hub's static key; not provisioned */
    PAIR_INIT_DERIVE_FAILED,
    PAIR_INIT_BAD_MAC,
    PAIR_INIT_REPLAY,         /* superframe at or below the durable ceiling */
    PAIR_INIT_RATE_LIMITED
} pair_init_rc_t;

typedef struct {
    uint32_t seen;            /* frames that parsed as a PAIR_INIT at all */
    uint32_t accepted;
    uint32_t bad_frame;
    uint32_t wrong_net;
    uint32_t not_addressed;
    uint32_t bad_mac;
    uint32_t replay;
    uint32_t rate_limited;
    uint32_t derive_failed;
    uint32_t z1_derivations;  /* the 103 ms scalar multiply; must stay at 1 */
    uint32_t last_superframe; /* of the last accepted init */
    uint32_t ceiling;         /* what a replay is compared against */
} pair_init_stats_t;

/* The device's own identity and the hub's static key, which together give Z1. */
typedef struct {
    const uint8_t *dev_priv;      /* 32 bytes */
    const uint8_t *hub_static_c;  /* 33 bytes, compressed SEC1 */
    uint32_t dev_id;
    uint32_t hub_id;
    uint16_t net_id;
} pair_init_ctx_t;

/** @brief Verifies an invitation; the order of the checks is the substance.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
pair_init_rc_t pair_init_verify(const pair_init_ctx_t *ctx, const uint8_t *frame,
                                uint8_t len, uint32_t now_ms, uint32_t ceiling,
                                uint32_t *superframe_out);

/** @brief Derives K_init from the stored Z1; exposed so a vector can run it.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int pair_init_key(const pair_init_ctx_t *ctx, uint8_t key_out[32]);
/** @brief Derives K_init from a supplied Z1, so a vector runs the live code.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
void pair_init_key_from_z1(const uint8_t z1[32], uint32_t hub_id, uint32_t dev_id,
                           uint8_t key_out[32]);
/** @brief Test seam: seeds Z1 so a published frame takes the real verify path.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
void pair_init_test_seed_z1(const uint8_t z1[32]);
/** @brief Derives Z1 once, ahead of the window that has to answer it.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int pair_init_prepare(const pair_init_ctx_t *ctx);

/** @brief Counters for invitations; each refusal reason is separate. */
void pair_init_stats(pair_init_stats_t *out);
/** @brief Clears the invitation counters. */
void pair_init_stats_reset(void);
/** @brief Drops the cached Z1, so a derivation count can be measured honestly. */
void pair_init_forget(void);
/** @brief Names an invitation result for the console. */
const char *pair_init_rc_name(pair_init_rc_t rc);
