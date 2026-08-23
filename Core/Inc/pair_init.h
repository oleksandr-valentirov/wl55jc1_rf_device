#pragma once

#include <stdint.h>

#include "radio_protocol.h"

/* The hub's addressed invitation, keyed from the two static keys.
 * radio_devices_docs/wl55_device/radio/pairing.md */

typedef enum {
    PAIR_INIT_OK = 0,
    PAIR_INIT_BAD_FRAME,      /* type, version or length */
    PAIR_INIT_WRONG_NET,      /* net_id or hub_id is another network's */
    PAIR_INIT_NOT_ADDRESSED,  /* dev_id names a different device */
    PAIR_INIT_BAD_MODE,       /* the frame's enrolment mode is not this device's */
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
    uint32_t bad_mode;
    uint32_t last_superframe; /* of the last accepted init */
    uint32_t ceiling;         /* what a replay is compared against */
} pair_init_stats_t;

/* The device's own identity and the hub's static key, which together give Z1. */
typedef struct {
    const uint8_t *dev_priv;      /* 32 bytes */
    uint32_t dev_id;
    uint32_t hub_id;
    uint16_t net_id;
    uint8_t  enrol_mode;          /* fixed at provisioning, never from the frame */
} pair_init_ctx_t;

/** @brief Verifies an invitation; the order of the checks is the substance.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
pair_init_rc_t pair_init_verify(const pair_init_ctx_t *ctx, const uint8_t *frame,
                                uint8_t len, uint32_t now_ms, uint32_t ceiling,
                                uint32_t *superframe_out, uint8_t *hub_static_out);

/** @brief Counters for invitations; each refusal reason is separate. */
void pair_init_stats(pair_init_stats_t *out);
/** @brief Clears the invitation counters. */
void pair_init_stats_reset(void);
/** @brief Drops the cached Z1, so a derivation count can be measured honestly. */
void pair_init_forget(void);
/** @brief Names an invitation result for the console. */
const char *pair_init_rc_name(pair_init_rc_t rc);
