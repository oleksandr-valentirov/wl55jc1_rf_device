/**
 * @file hublogic.c
 * @brief The hub's half of the enrolment, above phy.h and free of any chip.
 *
 * radio_devices_docs/radio/phy-seam.md
 */
#include <stddef.h>
#include <string.h>

#include "hublogic.h"
#include "phy.h"
#include "crypto.h"
#include "exchange.h"
#include "radio_protocol.h"
#include "radio_slots.h"

/* The board this build drives is a node; only the logic above phy.h is the hub's. */
#ifndef WL55_HUB_ID
#define WL55_HUB_ID          0x574C3535u
#endif
#ifndef WL55_HUB_DEV_ID
#define WL55_HUB_DEV_ID      0x751C5A3Bu
#endif
/* Above any ceiling the H755 has already written into the device's store.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#ifndef WL55_HUB_SF_START
#define WL55_HUB_SF_START    0x01000000u
#endif
/* 16 s, clear of the device's 10 s rate limit on accepted invitations.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#ifndef WL55_HUB_INVITE_EVERY
#define WL55_HUB_INVITE_EVERY  8u
#endif
/* The CM4 superloop's mailbox gap, reproduced on purpose; 0 is the clean run. */
#ifndef WL55_HUB_STALL_US
#define WL55_HUB_STALL_US    0u
#endif

_Static_assert(WL55_HUB_INVITE_EVERY % RADIO_PAIR_INIT_EVERY == 0u,
               "an invitation must land on a superframe the grid invites on");

/* The device refuses an accept further than this past its request; join.h owns it.
 * radio_devices_docs/radio/pairing.md */
#define HUB_ACCEPT_WINDOW_SF    8u

#define HUB_GRANT_SLOT          0u
#define HUB_GRANT_REPORT_EVERY  4u
/* One superframe covers the whole exchange; two leave the retry no clear air. */
#define HUB_EX_TIMEOUT_US       (2u * SUPERFRAME_US)

enum { HUB_EX_IDLE = 0, HUB_EX_WAIT_REQ, HUB_EX_WAIT_CONF };

static hub_view_t v;

static uint8_t  static_priv[X25519_PRIV_LEN], static_pub[X25519_PUB_LEN];
static uint8_t  have_static;

static uint32_t sf_start_us;
static uint8_t  grid_started;

static uint8_t  window_open;
static uint8_t  beacon_done;         /* one beacon or invitation a superframe, at the offset */

static uint8_t  ex_state;
static uint32_t ex_dev_id;
static uint32_t ex_deadline_us;
static uint32_t ex_req_superframe;
static exchange_keys_t ex_keys;

static uint32_t t_init_us, t_req_us, t_rsp_us, t_conf_us;

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t offset_us(void) {
    return phy_now_us() - sf_start_us;
}

static int elapsed(uint32_t deadline_us) {
    return (int32_t)(phy_now_us() - deadline_us) >= 0;
}

/* --- the frames --------------------------------------------------------- */

static int send_join_beacon(void) {
    radio_join_beacon_t b;

    b.type         = RADIO_FRAME_JOIN_BEACON;
    b.version      = RADIO_PROTO_VERSION;
    b.net_id       = RADIO_NET_ID;
    b.hub_id       = WL55_HUB_ID;
    b.superframe   = v.frame_counter;
    b.flags        = RADIO_JOIN_FLAG_WINDOW_OPEN;
    b.hop_channels = (uint8_t)RADIO_HOP_COUNT;
    return phy_transmit(&b, (uint8_t)sizeof(b), NULL);
}

static int send_pair_init(void) {
    radio_pair_init_t f;

    memset(&f, 0, sizeof(f));
    f.type       = RADIO_FRAME_PAIR_INIT;
    f.version    = RADIO_PAIR_INIT_VERSION;
    f.net_id     = RADIO_NET_ID;
    f.hub_id     = WL55_HUB_ID;
    f.dev_id     = WL55_HUB_DEV_ID;
    f.superframe = v.frame_counter;
    f.mode       = RADIO_ENROL_MODE_OPEN;
    memcpy(f.hub_static, static_pub, sizeof(f.hub_static));
    return phy_transmit(&f, (uint8_t)sizeof(f), NULL);
}

/* --- the exchange ------------------------------------------------------- */

static void ex_reset(void) {
    ex_state  = HUB_EX_IDLE;
    ex_dev_id = 0;
    memset(&ex_keys, 0, sizeof(ex_keys));
}

static void handle_pair_req(const uint8_t *rx, uint8_t len) {
    uint8_t eph_priv[X25519_PRIV_LEN], eph_pub[X25519_PUB_LEN];
    uint8_t z1[32], z2[32];
    uint8_t salt[EXCHANGE_SALT_LEN];
    uint8_t transcript[EXCHANGE_TRANSCRIPT_LEN];
    radio_pair_rsp_t rsp;
    const uint8_t *dev_pub   = rx + offsetof(radio_pair_req_t, pubkey);
    const uint8_t *dev_nonce = rx + offsetof(radio_pair_req_t, dev_nonce);
    uint32_t req_sf;
    uint8_t nz = 0;

    v.req_seen++;
    if (len != sizeof(radio_pair_req_t) || rx[1] != RADIO_PAIR_VERSION) {
        v.req_bad_frame++;
        return;
    }
    if ((uint16_t)((uint16_t)rx[2] | ((uint16_t)rx[3] << 8)) != RADIO_NET_ID ||
        get_le32(rx + 4) != WL55_HUB_ID || get_le32(rx + 8) != WL55_HUB_DEV_ID) {
        v.req_bad_ids++;
        return;
    }
    for (uint32_t i = 0; i < EXCHANGE_NONCE_LEN; i++)
        nz = (uint8_t)(nz | dev_nonce[i]);
    if (nz == 0u) {
        v.req_bad_nonce++;
        return;
    }
    req_sf = get_le32(rx + offsetof(radio_pair_req_t, superframe));

    if (crypto_x25519_keygen(eph_priv, eph_pub) != 0) {
        v.req_no_key++;
        return;
    }
    /* Hub term first in every concatenation, exactly as the device derives it.
     * radio_devices_docs/radio/pairing.md */
    if (crypto_x25519_ecdh(static_priv, dev_pub, z1) != 0 ||
        crypto_x25519_ecdh(eph_priv, dev_pub, z2) != 0) {
        v.req_bad_point++;
        return;
    }
    exchange_salt(WL55_HUB_ID, WL55_HUB_DEV_ID, req_sf, dev_nonce, salt);
    exchange_transcript(WL55_HUB_ID, WL55_HUB_DEV_ID, req_sf, dev_nonce,
                        static_pub, eph_pub, dev_pub, transcript);
    exchange_derive(z1, z2, salt, transcript, &ex_keys);
    memset(z1, 0, sizeof(z1));
    memset(z2, 0, sizeof(z2));
    memset(eph_priv, 0, sizeof(eph_priv));

    rsp.type    = RADIO_FRAME_PAIR_RSP;
    rsp.version = RADIO_PAIR_VERSION;
    rsp.hub_id  = WL55_HUB_ID;
    rsp.dev_id  = WL55_HUB_DEV_ID;
    memcpy(rsp.eph_pubkey, eph_pub, sizeof(rsp.eph_pubkey));
    memcpy(rsp.confirm, ex_keys.confirm_hub, sizeof(rsp.confirm));

    ex_req_superframe = req_sf;
    ex_dev_id         = WL55_HUB_DEV_ID;
    if (phy_transmit(&rsp, (uint8_t)sizeof(rsp), NULL) != 0) {
        v.rsp_tx_err++;
        ex_reset();
        return;
    }
    v.rsp_sent++;
    t_rsp_us = phy_now_us();
    v.req_to_rsp_us = t_rsp_us - t_req_us;
    ex_state       = HUB_EX_WAIT_CONF;
    ex_deadline_us = phy_now_us() + HUB_EX_TIMEOUT_US;
}

static void handle_pair_conf(const uint8_t *rx, uint8_t len) {
    radio_pair_accept_t acc;
    radio_pair_grant_t grant;
    uint8_t nonce[12];
    uint32_t sf = v.frame_counter;
    uint32_t w;

    v.conf_seen++;
    if (len != sizeof(radio_pair_conf_t) || rx[1] != RADIO_PAIR_VERSION) {
        v.conf_bad_frame++;
        return;
    }
    if (get_le32(rx + 2) != WL55_HUB_ID || get_le32(rx + 6) != WL55_HUB_DEV_ID) {
        v.conf_bad_ids++;
        return;
    }
    if (ex_state != HUB_EX_WAIT_CONF) {
        v.conf_no_exchange++;
        return;
    }
    if (!exchange_confirm_equal(rx + offsetof(radio_pair_conf_t, confirm),
                                ex_keys.confirm_dev)) {
        v.conf_mismatch++;
        ex_reset();
        return;
    }
    t_conf_us = phy_now_us();
    v.rsp_to_conf_us = t_conf_us - t_rsp_us;

    /* The device refuses an accept further than this from the request it made. */
    if ((uint32_t)(sf - ex_req_superframe) > HUB_ACCEPT_WINDOW_SF)
        sf = ex_req_superframe;

    grant.slot         = HUB_GRANT_SLOT;
    grant.report_every = HUB_GRANT_REPORT_EVERY;
    grant.flags        = 0u;
    for (uint32_t i = 0; i < sizeof(grant.hop_key); i += 4u) {
        if (crypto_rng_word(&w) != 0) {
            v.accept_tx_err++;
            ex_reset();
            return;
        }
        memcpy(grant.hop_key + i, &w, 4);
    }

    acc.type       = RADIO_FRAME_PAIR_ACCEPT;
    acc.version    = RADIO_PAIR_VERSION;
    acc.hub_id     = WL55_HUB_ID;
    acc.dev_id     = WL55_HUB_DEV_ID;
    acc.superframe = sf;
    acc.retry      = 0u;

    nonce[0] = (uint8_t)(sf >> 24); nonce[1] = (uint8_t)(sf >> 16);
    nonce[2] = (uint8_t)(sf >> 8);  nonce[3] = (uint8_t)sf;
    nonce[4] = (uint8_t)(WL55_HUB_DEV_ID >> 24);
    nonce[5] = (uint8_t)(WL55_HUB_DEV_ID >> 16);
    nonce[6] = (uint8_t)(WL55_HUB_DEV_ID >> 8);
    nonce[7] = (uint8_t)WL55_HUB_DEV_ID;
    nonce[8]  = RADIO_DIR_DOWNLINK;
    nonce[9]  = (uint8_t)(RADIO_NONCE_SLOT_UNSLOTTED >> 16);
    nonce[10] = (uint8_t)(RADIO_NONCE_SLOT_UNSLOTTED >> 8);
    nonce[11] = acc.retry;

    if (crypto_gcm_seal(ex_keys.session, nonce, (const uint8_t *)&acc,
                        RADIO_PAIR_ACCEPT_AAD_LEN, (const uint8_t *)&grant,
                        (uint16_t)sizeof(grant), acc.ct, acc.tag) != 0) {
        v.accept_tx_err++;
        ex_reset();
        return;
    }
    if (phy_transmit(&acc, (uint8_t)sizeof(acc), NULL) != 0) {
        v.accept_tx_err++;
        ex_reset();
        return;
    }
    v.accept_sent++;
    v.paired++;
    v.conf_to_accept_us = phy_now_us() - t_conf_us;
    ex_reset();
}

static void handle_frame(const uint8_t *rx, uint8_t len, int16_t rssi) {
    if (len == 0u)
        return;
    if (rx[0] == RADIO_FRAME_PAIR_REQ) {
        v.req_rssi_dbm = rssi;
        t_req_us = phy_now_us();
        v.init_to_req_us = t_req_us - t_init_us;
        handle_pair_req(rx, len);
        return;
    }
    if (rx[0] == RADIO_FRAME_PAIR_CONF) {
        v.conf_rssi_dbm = rssi;
        handle_pair_conf(rx, len);
        return;
    }
    /* A type byte, kept apart from any count of frames carrying it. */
    v.other_type = rx[0];
    v.other_frames++;
}

/* --- the grid ----------------------------------------------------------- */

static void on_superframe(void) {
    v.superframes++;
    beacon_done = 0;
}

static void window_service(void) {
    uint32_t off = offset_us();

    if (!window_open) {
        if (off < RADIO_UPLINK_OFFSET_US)
            return;
        if (off >= SUPERFRAME_US - RADIO_END_GUARD_US)
            return;
        if (phy_tune(RADIO_JOIN_HZ) != 0 || phy_listen() != 0)
            return;
        window_open = 1;
        v.windows++;
        return;
    }

    if (off >= SUPERFRAME_US - RADIO_END_GUARD_US) {
        phy_standby();
        window_open = 0;
        return;
    }

    /* One transmission a superframe, at the offset a joining device listens on.
     * radio_devices_docs/radio/joining.md */
    if (!beacon_done && off >= RADIO_JOIN_OFFSET_US) {
        beacon_done = 1;
        if ((v.frame_counter % WL55_HUB_INVITE_EVERY) == 0u &&
            ex_state == HUB_EX_IDLE) {
            if (send_pair_init() != 0) {
                v.init_tx_err++;
            } else {
                v.inits_sent++;
                t_init_us      = phy_now_us();
                ex_state       = HUB_EX_WAIT_REQ;
                ex_deadline_us = phy_now_us() + HUB_EX_TIMEOUT_US;
            }
        } else if ((v.frame_counter % 2u) == 0u) {
            if (send_join_beacon() == 0)
                v.beacons++;
        }
    }
}

static void poll_service(void) {
    phy_ev_t ev;

    if (!window_open)
        return;
    if (phy_poll(&ev) != 0)
        return;
    switch (ev.kind) {
    case PHY_EV_SYNC:  v.ev_sync++;  break;
    case PHY_EV_CRC:   v.ev_crc++;   break;
    case PHY_EV_FRAME:
        v.ev_frame++;
        handle_frame(ev.buf, ev.len, ev.rssi_dbm);
        break;
    default:
        break;
    }
}

static void stall(void) {
    uint32_t until;

    if (WL55_HUB_STALL_US == 0u)
        return;
    until = phy_now_us() + WL55_HUB_STALL_US;
    while (!elapsed(until))
        ;
}

void hub_init(void) {
    memset(&v, 0, sizeof(v));
    v.stall_us     = WL55_HUB_STALL_US;
    v.frame_counter = WL55_HUB_SF_START;
    have_static = (crypto_x25519_keygen(static_priv, static_pub) == 0) ? 1u : 0u;
    if (phy_init() != 0)
        return;
    sf_start_us  = phy_now_us();
    grid_started = 1;
}

void hub_service(void) {
    if (!grid_started || !have_static)
        return;

    if ((uint32_t)(phy_now_us() - sf_start_us) >= SUPERFRAME_US) {
        sf_start_us += SUPERFRAME_US;
        v.frame_counter++;
        on_superframe();
    }

    window_service();
    poll_service();

    if (ex_state != HUB_EX_IDLE && elapsed(ex_deadline_us)) {
        v.ex_timeouts++;
        ex_reset();
    }
    stall();
}

void hub_snapshot(hub_view_t *out) {
    *out = v;
}
