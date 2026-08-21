#pragma once

#include <stdint.h>

#include "exchange.h"
#include "pairing.h"
#include "pair_init.h"

/* The four-frame exchange, device side, all of it on the fixed join channel.
 * radio_devices_docs/wl55_device/radio/pairing.md */

/* A window, not a replay check: it bounds a recording, dev_nonce makes it fresh.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#define JOIN_ACCEPT_WINDOW_SF  8u

/* Every stage counted apart, so a failure names itself.
 * radio_devices_docs/wl55_device/radio/pairing.md */
typedef struct {
    uint32_t beacon_timeout;     /* the receive window closed with nothing in it */
    uint32_t beacon_other;       /**< heard something that was not a usable join beacon */
    uint32_t beacon_crc_err;     /**< the only side of the link where a CRC failure is visible */
    uint32_t rsp_crc_err;
    uint32_t accept_crc_err;
    uint8_t  last_len;           /* of whatever last landed, 0 if nothing */
    int16_t  last_rssi_dbm;
    uint8_t  last_type;
    uint8_t  invite_refused;     /**< which pair_init_rc_t refused, so -30 names its reason */
    uint8_t  rsp_len;            /**< the response as it arrived, recorded before it is judged */
    uint8_t  rsp_type;
    uint8_t  rsp_version;
    int16_t  rsp_rssi_dbm;
    uint32_t beacon_to_req_us;   /**< measured: the hub retunes ~100 ms after its beacon */
    uint32_t beacon_closed;      /* heard, but the operator's window is not open */
    uint32_t req_sent;
    uint32_t rsp_timeout;        /* nothing at all arrived */
    uint32_t rsp_heard;          /* a frame of the response type arrived */
    uint32_t rsp_skipped;        /* wrong type in the window - usually the beacon */
    uint32_t accept_skipped;
    uint8_t  rsp_other_type;     /* what the last skipped one was, so it is nameable */
    uint8_t  accept_other_type;
    uint32_t rsp_bad_frame;      /* length, type or version */
    uint32_t rsp_wrong_ids;
    uint32_t rsp_bad_point;
    uint32_t rsp_eph_is_static;
    uint32_t rsp_confirm_bad;
    uint32_t conf_sent;
    uint32_t accept_timeout;
    uint32_t accept_heard;
    uint32_t accept_bad_frame;
    uint32_t accept_outside_window;
    uint32_t accept_bad_tag;
    uint32_t paired;
    uint32_t store_failed;   /* paired on air, not on flash - a reset loses it */
} join_stats_t;

typedef struct {
    uint8_t  hub_static[33];     /* provisioned out of band; Z1 needs it */
    uint8_t  have_hub_static;
    uint32_t superframe;         /* from the join beacon */
    uint8_t  slot;               /* granted */
    uint8_t  report_every;
    uint8_t  hop_key[16];        /* network-wide, from PAIR_ACCEPT */
    uint8_t  session[16];
    uint32_t dev_id_be;          /* the nonce input, kept beside what it seals */
    uint8_t  paired;
} join_result_t;

/* A code names the stage; `join stats` names the reason - -1 alone covers three. */

/** @brief Whether the hub's static key is provisioned, loading it from flash. */
int  join_hub_static_ready(join_result_t *res);
/** @brief Reloads a grant from flash, so a reset does not undo pairing. */
int  join_restore(join_result_t *res);
/** @brief Joins from a cleartext join beacon, the v2 trigger. */
int  join_run(pairing_ctx_t *pair, join_result_t *out, uint32_t timeout_ms);
/** @brief Joins from an addressed, MACed PAIR_INIT, the v3 trigger.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int  join_run_invited(pairing_ctx_t *pair, join_result_t *out,
                      uint32_t timeout_ms, const pair_init_ctx_t *ic,
                      uint32_t ceiling);

/* A length sweep, not a pairing attempt: only the PHY below the type byte runs.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#define JOIN_PROBE_MAX   64u    /* the hub's RFM69 FIFO is 66 bytes */
#define JOIN_PROBE_TYPE  0x7Eu
/** @brief Times a request of a given length on air, without pairing. */
int  join_probe(uint8_t len, uint32_t timeout_ms, uint32_t *air_us,
                uint32_t *delay_us);
/** @brief Returns the last request built, for publishing a vector. */
uint8_t join_last_request(uint8_t *out, uint8_t cap);
/** @brief Counters for the exchange; every refusal is counted separately. */
void join_stats(join_stats_t *out);
/** @brief Clears the exchange counters. */
void join_stats_reset(void);

typedef struct {
    uint8_t accept_ok;
    uint8_t grant_ok;
    uint8_t hop_key_ok;
    uint8_t forged_rejected;
    uint8_t stale_rejected;
    uint8_t confirm_ok;
    uint8_t req_built_ok;
    uint8_t rsp_parsed_ok;
    uint8_t conf_built_ok;       /**< 0 wrong, 1 matches, 2 not reached; never-ran is not FAIL */
    uint8_t eph_static_rejected;
    int     accept_rc;
    uint8_t nonce[12];
    uint8_t aad_len;
    uint8_t ct_len;
} join_selftest_t;

/** @brief Runs the accept path against the published frame, sparing the counters.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int join_selftest(join_selftest_t *r);
