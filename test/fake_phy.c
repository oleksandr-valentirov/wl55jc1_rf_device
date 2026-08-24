/**
 * @file fake_phy.c
 * @brief A radio that is not there, and one device on the other side of it.
 *
 * `hublogic.c` reaches the air through the nine operations of `phy.h` and
 * nothing else, so a host build needs only these. The clock is the test's, and
 * it advances only when the test says so - which is what makes an exchange
 * that takes four seconds of air take microseconds here.
 *
 * The device is scripted rather than reused: `join.c` blocks on a real radio.
 * What it reproduces faithfully is the one thing this suite is about - **it
 * holds its confirmation to RADIO_PAIR_CONF_REGION past the invitation**, which
 * is ADR-0026 and is what the hub half was never taught.
 *
 * radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <string.h>

#include "fake_phy.h"
#include "crypto.h"
#include "exchange.h"
#include "radio_protocol.h"
#include "radio_slots.h"

fake_phy_t fp;

/* --- the contract, as this file implements it --------------------------- */

int phy_init(void)  { fp.inited = 1; return fp.init_fails ? -1 : 0; }
int phy_tune(uint32_t hz) { fp.hz = hz; return 0; }
int phy_standby(void) { fp.listening = 0; return 0; }
int phy_listen(void) { fp.listening = 1; return 0; }
int16_t phy_rssi_now(void) { return 0; }
uint32_t phy_now_us(void) { return fp.now_us; }

/* --- the device on the other side --------------------------------------- */

static void dev_pending(const void *frame, uint8_t len, uint32_t at_us) {
    memcpy(fp.pending, frame, len);
    fp.pending_len = len;
    fp.pending_at  = at_us;
}

/* Everything the device needs to answer, computed the way the device does. */
static void dev_on_init(const radio_pair_init_t *in) {
    radio_pair_req_t req;

    fp.dev_saw_init++;
    fp.t_init_us = fp.now_us;
    memcpy(fp.hub_static, in->hub_static, sizeof(fp.hub_static));
    fp.req_superframe = in->superframe;
    if (crypto_x25519_keygen(fp.dev_priv, fp.dev_pub) != 0)
        return;

    memset(&req, 0, sizeof(req));
    req.type       = RADIO_FRAME_PAIR_REQ;
    req.version    = RADIO_PAIR_VERSION;
    req.net_id     = in->net_id;
    req.hub_id     = in->hub_id;
    req.dev_id     = in->dev_id;
    req.superframe = in->superframe;
    for (int i = 0; i < 8; i++)
        fp.dev_nonce[i] = (uint8_t)(0xA0 + i);
    memcpy(req.dev_nonce, fp.dev_nonce, sizeof(req.dev_nonce));
    memcpy(req.pubkey, fp.dev_pub, sizeof(req.pubkey));
    fp.hub_id = in->hub_id;
    fp.dev_id = in->dev_id;
    dev_pending(&req, (uint8_t)sizeof(req), fp.now_us + RADIO_PAIR_REQ_LEAD_US);
}

static void dev_on_rsp(const radio_pair_rsp_t *rsp) {
    uint8_t z1[32], z2[32], salt[EXCHANGE_SALT_LEN];
    uint8_t transcript[EXCHANGE_TRANSCRIPT_LEN];
    exchange_keys_t k;
    radio_pair_conf_t conf;
    uint32_t due;

    fp.dev_saw_rsp++;
    /* The device's own acceptance path, in its order.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (rsp->hub_id != fp.hub_id || rsp->dev_id != fp.dev_id) {
        fp.dev_rsp_wrong_ids++;
        return;
    }
    if (exchange_eph_is_static(rsp->eph_pubkey, fp.hub_static)) {
        fp.dev_rsp_eph_static++;
        return;
    }
    if (crypto_x25519_ecdh(fp.dev_priv, fp.hub_static, z1) != 0 ||
        crypto_x25519_ecdh(fp.dev_priv, rsp->eph_pubkey, z2) != 0) {
        fp.dev_rsp_bad_point++;
        return;
    }
    exchange_salt(fp.hub_id, fp.dev_id, fp.req_superframe, fp.dev_nonce, salt);
    exchange_transcript(fp.hub_id, fp.dev_id, fp.req_superframe, fp.dev_nonce,
                        fp.hub_static, rsp->eph_pubkey, fp.dev_pub, transcript);
    exchange_derive(z1, z2, salt, transcript, &k);
    if (!exchange_confirm_equal(rsp->confirm, k.confirm_hub)) {
        fp.dev_rsp_mismatch++;
        return;
    }

    memset(&conf, 0, sizeof(conf));
    conf.type    = RADIO_FRAME_PAIR_CONF;
    conf.version = RADIO_PAIR_VERSION;
    conf.hub_id  = fp.hub_id;
    conf.dev_id  = fp.dev_id;
    memcpy(conf.confirm, k.confirm_dev, sizeof(conf.confirm));
    if (fp.corrupt_confirm)
        conf.confirm[0] ^= 0xFFu;

    /* ADR-0026: held to its region past the **invitation**, not past this frame. */
    due = fp.t_init_us + fp.conf_region * SUPERFRAME_US + fp.conf_extra_us;
    if (fp.confirm_immediately)
        due = fp.now_us + RADIO_PAIR_DEV_TURNAROUND_US;
    dev_pending(&conf, (uint8_t)sizeof(conf), due);
}

/* What the device does with a frame the hub transmitted. */
static void dev_receive(const uint8_t *buf, uint8_t len) {
    if (len == 0u)
        return;
    if (buf[0] == RADIO_FRAME_PAIR_INIT && len == sizeof(radio_pair_init_t)) {
        radio_pair_init_t in;

        memcpy(&in, buf, sizeof(in));
        dev_on_init(&in);
        return;
    }
    if (buf[0] == RADIO_FRAME_PAIR_RSP && len == sizeof(radio_pair_rsp_t)) {
        radio_pair_rsp_t rsp;

        memcpy(&rsp, buf, sizeof(rsp));
        dev_on_rsp(&rsp);
        return;
    }
    if (buf[0] == RADIO_FRAME_PAIR_ACCEPT && len == sizeof(radio_pair_accept_t)) {
        fp.dev_saw_accept++;
        fp.accept_at_us = fp.now_us;
    }
}

int phy_transmit(const void *payload, uint8_t len, uint32_t *air_us) {
    const uint8_t *b = (const uint8_t *)payload;

    if (len > PHY_MAX_PAYLOAD)
        return -2;
    if (fp.tx_fails)
        return -1;
    fp.tx_count++;
    fp.last_tx_type = b[0];
    /* Air time is charged, so a hub that transmits inside a window spends it. */
    fp.now_us += RADIO_AIR_START_TO_END_US(len);
    if (air_us != NULL)
        *air_us = RADIO_AIR_START_TO_END_US(len);
    if (fp.deaf_device)
        return 0;
    dev_receive(b, len);
    return 0;
}

/* A frame is delivered only while the hub is listening, which is the point. */
int phy_poll(phy_ev_t *ev) {
    memset(ev, 0, sizeof(*ev));
    ev->kind     = PHY_EV_NONE;
    ev->lna_gain = PHY_LNA_UNKNOWN;
    if (fp.pending_len == 0u)
        return 0;
    if ((int32_t)(fp.now_us - fp.pending_at) < 0)
        return 0;
    if (!fp.listening) {
        /* Counted, never queued: a frame nobody was listening for is lost. */
        fp.missed_while_deaf++;
        fp.pending_len = 0;
        return 0;
    }
    memcpy(ev->buf, fp.pending, fp.pending_len);
    ev->len      = fp.pending_len;
    ev->kind     = PHY_EV_FRAME;
    ev->rssi_dbm = -42;
    ev->sync_seq = ++fp.sync_seq;
    ev->sync_valid = 1;
    ev->sync_us  = fp.now_us;
    fp.pending_len = 0;
    fp.delivered++;
    return 0;
}

/* The air does not wait for a listener: one call per loop step. */
void fake_phy_tick(void) {
    if (fp.pending_len == 0u)
        return;
    if ((int32_t)(fp.now_us - fp.pending_at) < 0)
        return;
    if (!fp.listening) {
        fp.missed_while_deaf++;
        fp.pending_len = 0;
    }
}

void fake_phy_reset(void) {
    memset(&fp, 0, sizeof(fp));
    fp.conf_region = RADIO_PAIR_CONF_REGION;
    fp.now_us = 1000u;
}
