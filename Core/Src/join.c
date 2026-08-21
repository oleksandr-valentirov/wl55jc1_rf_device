/* The device's half of the four-frame exchange, written from pair_v2 and the
 * shared header rather than from the hub's source.
 *
 * Ordering is the design: every cheap refusal happens before the two scalar
 * multiplications, because a frame addressed elsewhere must not cost 200 ms of
 * PKA, and the confirmation is checked before PAIR_CONF goes out, because
 * answering an exchange this end cannot verify is how a relay gets a device to
 * talk to it. */
#include <string.h>

#include "join.h"
#include "crypto.h"
#include "radio.h"
#include "radio_protocol.h"
#include "timebase.h"
#include "store.h"
#include "pair_init.h"
#include "pair_v2.h"
#include "wire_v3.h"

static join_stats_t stats;
static uint8_t last_req[sizeof(radio_pair_req_t)];

/* One place, because `join show` reported NOT SET while join_run found the key
 * in flash and proceeded - a display that contradicted the behaviour beside it,
 * which is how an operator sets a key that is already there or concludes the
 * device is not ready when it is. The key is provisioned out of band and
 * nothing on air supplies it, so losing it to a reflash costs a window. */
int join_hub_static_ready(join_result_t *res) {
    store_state_t st;
    if (res->have_hub_static)
        return 1;
    if (store_init(&st) != 0 || !st.valid)
        return 0;
    if (st.hub_static[0] != 0x02u && st.hub_static[0] != 0x03u)
        return 0;
    memcpy(res->hub_static, st.hub_static, sizeof(res->hub_static));
    res->have_hub_static = 1;
    return 1;
}

void join_stats(join_stats_t *out) { *out = stats; }

uint8_t join_last_request(uint8_t *out, uint8_t cap) {
    uint8_t n = cap < sizeof(last_req) ? cap : (uint8_t)sizeof(last_req);
    memcpy(out, last_req, n);
    return n;
}
void join_stats_reset(void) { memset(&stats, 0, sizeof(stats)); }

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


static void compress_pub(const uint8_t *sec1_65, uint8_t *out33) {
    out33[0] = (uint8_t)(0x02u | (sec1_65[P256_PUB_LEN - 1] & 1u));
    memcpy(out33 + 1, sec1_65 + 1, 32);
}

/* Wire fields are little-endian; everything entering the crypto layer is
 * big-endian. Both rules are live inside one frame, which is why the shared
 * vectors pin whole frames rather than a field list. */
static void build_request(const pairing_ctx_t *p, uint32_t superframe, uint8_t *out) {
    radio_pair_req_t f;
    f.type       = RADIO_FRAME_PAIR_REQ;
    f.version    = RADIO_PROTO_VERSION;
    f.net_id     = p->net_id;
    f.hub_id     = p->hub_id;
    f.dev_id     = p->dev_id;
    f.superframe = superframe;
    memcpy(f.dev_nonce, p->dev_nonce, sizeof(f.dev_nonce));
    compress_pub(p->pub, f.pubkey);
    memcpy(out, &f, sizeof(f));
}

static void build_confirm(const pairing_ctx_t *p, const uint8_t *confirm_dev,
                          uint8_t *out) {
    radio_pair_conf_t c;
    c.type    = RADIO_FRAME_PAIR_CONF;
    c.version = RADIO_PROTO_VERSION;
    c.hub_id  = p->hub_id;
    c.dev_id  = p->dev_id;
    memcpy(c.confirm, confirm_dev, sizeof(c.confirm));
    memcpy(out, &c, sizeof(c));
}

static int listen_join_beacon(uint32_t timeout_ms, radio_join_beacon_t *b) {
    uint8_t rx[64];
    radio_rx_info_t info = {0};

    info.timeout_us = timeout_ms * 1000u;
    int rrc = radio_receive(rx, sizeof(rx), &info);
    if (rrc != 0) {
        /* -3 is a frame that arrived and failed its CRC, which is a completely
         * different fact from silence: it says the carrier, the bitrate and the
         * sync word are all right and only the checksum disagrees. Collapsing
         * the two is what made the first attempt undiagnosable. */
        if (rrc == -3)
            stats.beacon_crc_err++;
        else
            stats.beacon_timeout++;
        stats.last_len = 0;
        return -1;
    }
    /* Recorded before anything is judged: a length and an RSSI say whether the
     * hub is audible at all, which no refusal counter can. */
    stats.last_len = info.len;
    stats.last_rssi_dbm = info.rssi_dbm;
    stats.last_type = info.len ? rx[0] : 0u;
    if (info.len != sizeof(*b) || rx[0] != RADIO_FRAME_JOIN_BEACON ||
        rx[1] != RADIO_PROTO_VERSION) {
        stats.beacon_other++;
        return -1;
    }
    memcpy(b, rx, sizeof(*b));
    if (!(b->flags & RADIO_JOIN_FLAG_WINDOW_OPEN)) {
        /* Counted apart from silence: a closed window is a working hub and an
         * operator who has not pressed the button, which is not a fault. */
        stats.beacon_closed++;
        return -2;
    }
    return 0;
}

/* The hub's join beacon shares this channel every second superframe, so the
 * first frame in a response window is usually not the answer. Refusing it must
 * not close the window - that loses every reply sent more than one beacon
 * period away, which is most of them. */
static int receive_until(uint8_t *rx, uint8_t cap, radio_rx_info_t *info,
                         uint8_t want_type, uint32_t t0, uint32_t budget_us,
                         uint32_t *skipped, uint32_t *crc_err,
                         uint8_t *last_other) {
    for (;;) {
        /* Written as a difference: TIM2 wraps every ~71 min mid-exchange. */
        uint32_t spent = micros() - t0;
        if (spent >= budget_us)
            return -1;
        info->timeout_us = budget_us - spent;
        int rc = radio_receive(rx, cap, info);
        if (rc != 0) {
            /* A corrupt frame is not silence, and it has not used the window up. */
            if (rc == -3) { (*crc_err)++; continue; }
            return -2;
        }
        if (info->len > 0u && rx[0] == want_type)
            return 0;
        *last_other = info->len > 0u ? rx[0] : 0u;
        (*skipped)++;
    }
}

/* The superframe is read from `res`, never passed. It used to be a parameter,
 * and a caller that updated the request's source and not the transcript's sent
 * one number and hashed another - with every check before the confirmation
 * passing, because only the confirmation is about the two sides agreeing. One
 * source is the fix; a test that catches the divergence is not. */
static int handle_response(pairing_ctx_t *p, join_result_t *res,
                           const uint8_t *rx, uint8_t len,
                           exchange_keys_t *keys) {
    const uint32_t req_superframe = res->superframe;
    uint8_t peer[P256_PUB_LEN];
    uint8_t z1[32], z2[32];
    uint8_t transcript[EXCHANGE_TRANSCRIPT_LEN];
    uint8_t salt[EXCHANGE_SALT_LEN];
    uint8_t dev_c[33];

    if (len != sizeof(radio_pair_rsp_t) || rx[0] != RADIO_FRAME_PAIR_RSP ||
        rx[1] != RADIO_PROTO_VERSION) {
        stats.rsp_bad_frame++;
        return -3;
    }
    if (get_le32(rx + 2) != p->hub_id || get_le32(rx + 6) != p->dev_id) {
        /* Before the PKA, not after: two devices can be joining in one window
         * and the other one's response must cost nothing. */
        stats.rsp_wrong_ids++;
        return -4;
    }

    const uint8_t *eph_c = rx + offsetof(radio_pair_rsp_t, eph_pubkey);
    const uint8_t *confirm = rx + offsetof(radio_pair_rsp_t, confirm);

    /* Rejects the hub's static key reused as an ephemeral, and nothing else.
     * A recorded response replays past this untouched - dev_nonce is what makes
     * that fail, not this. */
    if (exchange_eph_is_static(eph_c, res->hub_static)) {
        stats.rsp_eph_is_static++;
        return -5;
    }
    if (crypto_p256_decompress(eph_c[0], eph_c + 1, peer) != 0) {
        stats.rsp_bad_point++;
        return -6;
    }

    uint8_t hub_static_full[P256_PUB_LEN];
    if (crypto_p256_decompress(res->hub_static[0], res->hub_static + 1,
                               hub_static_full) != 0) {
        stats.rsp_bad_point++;
        return -6;
    }
    /* Z1 authenticates the hub, Z2 supplies its freshness. Hub term first in
     * every concatenation; the order is a whole-key error with no symptom. */
    if (crypto_p256_ecdh(p->priv, hub_static_full, z1) != 0 ||
        crypto_p256_ecdh(p->priv, peer, z2) != 0) {
        stats.rsp_bad_point++;
        return -6;
    }

    compress_pub(p->pub, dev_c);
    exchange_salt(p->hub_id, p->dev_id, req_superframe, p->dev_nonce, salt);
    exchange_transcript(p->hub_id, p->dev_id, req_superframe, p->dev_nonce,
                        res->hub_static, eph_c, dev_c, transcript);
    exchange_derive(z1, z2, salt, transcript, keys);
    memset(z1, 0, sizeof(z1));
    memset(z2, 0, sizeof(z2));

    if (!exchange_confirm_equal(confirm, keys->confirm_hub)) {
        /* Nothing goes out after this fails. Answering an exchange this end
         * cannot verify is how a relay gets a device to talk to it. */
        memset(keys, 0, sizeof(*keys));
        stats.rsp_confirm_bad++;
        return -7;
    }
    return 0;
}

/* Kept, not debug scaffolding: if the first on-air exchange fails its tag, the
 * fastest thing either side can do is read its nonce out loud. Nothing else
 * distinguishes a wrong key from a wrong nonce assembly. */
static uint8_t last_accept_nonce[12];
static uint8_t last_accept_ct_len;

static int handle_accept(join_result_t *res, const uint8_t *rx, uint8_t len,
                         const exchange_keys_t *keys) {
    const uint32_t req_superframe = res->superframe;
    uint8_t nonce[12];
    radio_pair_grant_t grant;
    /* The grant carries the hop key inside it. Adding sizeof(hop_key) on top
     * of it double-counted, gave a 35-byte body, and failed the tag on a frame
     * with nothing wrong - which on air is indistinguishable from a radio
     * fault. Caught by opening the hub's published frame rather than one built
     * on this side, which would have agreed with the same mistake. */
    uint8_t plain[sizeof(radio_pair_grant_t)];
    _Static_assert(sizeof(radio_pair_grant_t) == 19u,
                   "the sealed grant is 19 bytes; pair_v2 pins the frame at 50");

    if (len != sizeof(radio_pair_accept_t) || rx[0] != RADIO_FRAME_PAIR_ACCEPT ||
        rx[1] != RADIO_PROTO_VERSION) {
        stats.accept_bad_frame++;
        return -8;
    }
    uint32_t sf = get_le32(rx + offsetof(radio_pair_accept_t, superframe));
    uint8_t retry = rx[offsetof(radio_pair_accept_t, retry)];
    /* Cheap, and before the tag. The exchange lives inside one quiesce, so a
     * frame claiming to be far from the request is a recording or a lost
     * window. This bounds a replay; it does not make the exchange fresh. */
    if ((int32_t)(sf - req_superframe) < 0 ||
        (uint32_t)(sf - req_superframe) > JOIN_ACCEPT_WINDOW_SF) {
        stats.accept_outside_window++;
        return -9;
    }

    nonce[0] = (uint8_t)(sf >> 24); nonce[1] = (uint8_t)(sf >> 16);
    nonce[2] = (uint8_t)(sf >> 8);  nonce[3] = (uint8_t)sf;
    nonce[4] = (uint8_t)(res->dev_id_be >> 24); nonce[5] = (uint8_t)(res->dev_id_be >> 16);
    nonce[6] = (uint8_t)(res->dev_id_be >> 8);  nonce[7] = (uint8_t)res->dev_id_be;
    nonce[8] = RADIO_DIR_DOWNLINK;
    nonce[9]  = (uint8_t)(RADIO_NONCE_SLOT_UNSLOTTED >> 16);
    nonce[10] = (uint8_t)(RADIO_NONCE_SLOT_UNSLOTTED >> 8);
    nonce[11] = retry;

    memcpy(last_accept_nonce, nonce, sizeof(last_accept_nonce));
    last_accept_ct_len = (uint8_t)sizeof(plain);

    /* Zeroed before the copy: HAL_CRYP_Decrypt leaves the unused bytes of a
     * partial final word unmasked, and 19 is not a multiple of four. */
    memset(plain, 0, sizeof(plain));
    if (crypto_gcm_open(keys->session, nonce, rx, RADIO_PAIR_ACCEPT_AAD_LEN,
                        rx + RADIO_PAIR_ACCEPT_AAD_LEN, sizeof(plain), plain,
                        rx + RADIO_PAIR_ACCEPT_AAD_LEN + sizeof(plain)) != 0) {
        stats.accept_bad_tag++;
        return -10;
    }
    memcpy(&grant, plain, sizeof(grant));
    if (grant.slot >= RADIO_SLOT_COUNT || grant.report_every == 0u) {
        stats.accept_bad_frame++;
        return -8;
    }
    res->slot = grant.slot;
    res->report_every = grant.report_every;
    memcpy(res->hop_key, grant.hop_key, sizeof(res->hop_key));
    memcpy(res->session, keys->session, sizeof(res->session));
    res->paired = 1;
    stats.paired++;
    return 0;
}

static int join_run_ex(pairing_ctx_t *p, join_result_t *res, uint32_t timeout_ms,
                       const pair_init_ctx_t *ic, uint32_t ceiling) {
    uint8_t frame[sizeof(radio_pair_req_t)];
    uint8_t rx[64];
    /* Zeroed: the invited path never fills it, and an uninitialised superframe
     * read out of here is a garbage transcript rather than a visible error. */
    radio_join_beacon_t beacon = {0};
    radio_rx_info_t info = {0};
    exchange_keys_t keys;
    int rc;

    if (!join_hub_static_ready(res))
        return -20;
    if (!p->have_key && pairing_keygen(p) != 0)
        return -21;
    /* A full configure, not a retune: the join channel needs the protocol sync
     * word, and `radio init` leaves this board in bench mode by default. */
    if (radio_configure(radio_join_slot()) != 0)
        return -22;

    /* Two ways in, one exchange behind them. v2 takes the cleartext beacon;
     * v3 takes an addressed invitation whose MAC has already been checked, and
     * the difference is that the second one cannot be forged into starting an
     * exchange. Everything after this point is identical, deliberately - the
     * trigger changed, the protocol did not. */
    uint32_t t_beacon;
    if (ic != NULL) {
        uint8_t inv[sizeof(radio_pair_init_t)];
        uint32_t sf = 0;
        /* Before the receive, not after the frame. Z1 is a function of two
         * static keys, so it can be paid for at any time - and paid for after
         * the invitation it puts 103 ms between the trigger and the request,
         * which measured 152 ms on air against the ~25 ms the hub waits. */
        if (pair_init_prepare(ic) != 0)
            return -31;
        rc = receive_until(inv, sizeof(inv), &info, RADIO_FRAME_PAIR_INIT,
                           micros(), timeout_ms * 1000u, &stats.rsp_skipped,
                           &stats.beacon_crc_err, &stats.last_type);
        if (rc != 0) {
            stats.beacon_timeout++;
            return -1;
        }
        t_beacon = micros();
        pair_init_rc_t irc = pair_init_verify(ic, inv, info.len, millis_hw(),
                                              ceiling, &sf);
        if (irc != PAIR_INIT_OK) {
            stats.invite_refused = (uint8_t)irc;
            return -30;
        }
        p->hub_id = (uint32_t)inv[4] | ((uint32_t)inv[5] << 8) |
                    ((uint32_t)inv[6] << 16) | ((uint32_t)inv[7] << 24);
        p->net_id = (uint16_t)((uint16_t)inv[2] | ((uint16_t)inv[3] << 8));
        res->superframe = sf;
        /* Only after the MAC, and only forwards - the same rule the ceiling
         * follows, because both are written off the same frame. */
        (void)store_save_init_ceiling(sf);
    } else {
        rc = listen_join_beacon(timeout_ms, &beacon);
        if (rc != 0)
            return rc;
        t_beacon = micros();
        p->hub_id  = beacon.hub_id;
        p->net_id  = beacon.net_id;
        res->superframe = beacon.superframe;
    }
    /* Kept, because verifying a PAIR_INIT after a reset needs the network it
     * must claim to be from - and PAIR_INIT exists to replace the very beacon
     * that carries it. Writes only when the value moves. */
    (void)store_save_network(p->hub_id, p->net_id);

    /* Fresh per attempt, and refused rather than sent as zero: a zero nonce
     * restores the replay this field exists to remove. */
    if (pairing_new_nonce(p) != 0)
        return -23;
    build_request(p, res->superframe, frame);
    memcpy(last_req, frame, sizeof(last_req));
    if (radio_send(frame, sizeof(frame), NULL) != 0)
        return -24;
    stats.beacon_to_req_us = micros() - t_beacon;
    stats.req_sent++;

    uint32_t t_req = micros();
    rc = receive_until(rx, sizeof(rx), &info, RADIO_FRAME_PAIR_RSP, t_req,
                       timeout_ms * 1000u, &stats.rsp_skipped,
                       &stats.rsp_crc_err, &stats.rsp_other_type);
    if (rc != 0) {
        stats.rsp_timeout++;
        return -12;
    }
    /* "A frame of the response type arrived", NOT "a valid PAIR_RSP arrived" -
     * the ids, the point and the confirmation are all still ahead. */
    stats.rsp_heard++;
    stats.rsp_len     = info.len;
    stats.rsp_rssi_dbm = info.rssi_dbm;
    stats.rsp_type    = info.len > 0u ? rx[0] : 0u;
    stats.rsp_version = info.len > 1u ? rx[1] : 0u;

    /* res->superframe, not beacon.superframe. The request carries the first and
     * the salt and transcript must hash the same number - and on the invited
     * path the beacon struct is never filled at all. Changing where the request
     * gets its value and not where the transcript gets it produced a confirm
     * failure with every check before it passing, because every check before it
     * is about the frame rather than about agreeing on a number. */
    rc = handle_response(p, res, rx, info.len, &keys);
    if (rc != 0)
        return rc;

    uint8_t conf[sizeof(radio_pair_conf_t)];
    build_confirm(p, keys.confirm_dev, conf);
    if (radio_send(conf, sizeof(conf), NULL) != 0)
        return -24;
    stats.conf_sent++;

    uint32_t t_conf = micros();
    rc = receive_until(rx, sizeof(rx), &info, RADIO_FRAME_PAIR_ACCEPT, t_conf,
                       timeout_ms * 1000u, &stats.accept_skipped,
                       &stats.accept_crc_err, &stats.accept_other_type);
    if (rc != 0) {
        stats.accept_timeout++;
        memset(&keys, 0, sizeof(keys));
        return -11;
    }
    stats.accept_heard++;
    res->dev_id_be = p->dev_id;
    rc = handle_accept(res, rx, info.len, &keys);
    memset(&keys, 0, sizeof(keys));
    if (rc == 0 && store_save_pairing(res->session, res->hop_key, res->slot,
                                      res->report_every) != 0) {
        /* Reported, not swallowed: the exchange did work and the keys are live
         * in RAM, but a reset from here is an unpaired device that an operator
         * has to visit. */
        stats.store_failed++;
        return -13;
    }
    return rc;
}

/* One fact, one source. `join show` reading RAM while `join_run` read flash is
 * how an operator concludes a device is not paired when it is. */
int join_restore(join_result_t *res) {
    store_state_t st;
    if (store_init(&st) != 0 || !st.valid || st.report_every == 0u)
        return 0;
    if (st.hub_static[0] != 0x02u && st.hub_static[0] != 0x03u)
        return 0;
    memcpy(res->hub_static, st.hub_static, sizeof(res->hub_static));
    res->have_hub_static = 1;
    memcpy(res->session, st.session, sizeof(res->session));
    memcpy(res->hop_key, st.hop_key, sizeof(res->hop_key));
    res->slot         = st.slot;
    res->report_every = st.report_every;
    res->dev_id_be    = st.dev_id;   /* the nonce input, restored with the key */
    res->paired       = 1;
    return 1;
}

/* Same rendezvous, same channel, only the length varies - an untimed frame meets
 * the hub's 5% window one time in twenty and reads as a failure. */
int join_run(pairing_ctx_t *p, join_result_t *res, uint32_t timeout_ms) {
    return join_run_ex(p, res, timeout_ms, NULL, 0u);
}

int join_run_invited(pairing_ctx_t *p, join_result_t *res, uint32_t timeout_ms,
                     const pair_init_ctx_t *ic, uint32_t ceiling) {
    return join_run_ex(p, res, timeout_ms, ic, ceiling);
}

int join_probe(uint8_t len, uint32_t timeout_ms, uint32_t *air_us,
               uint32_t *delay_us) {
    uint8_t frame[JOIN_PROBE_MAX];
    radio_join_beacon_t beacon;
    int rc;

    if (len < 4u || len > sizeof(frame))
        return -25;
    if (radio_configure(radio_join_slot()) != 0)
        return -22;
    rc = listen_join_beacon(timeout_ms, &beacon);
    if (rc != 0)
        return rc;

    uint32_t t_beacon = micros();
    /* Outside the protocol's type space deliberately: the hub must count this at
     * sync and CRC and refuse it above, never mistake it for a frame to act on. */
    frame[0] = JOIN_PROBE_TYPE;
    frame[1] = RADIO_PROTO_VERSION;
    /* Not a constant fill: with no whitening a run of equal bytes is what the
     * bit slicer fails on, and that is the thing being measured. */
    for (uint8_t i = 2u; i < len; i++)
        frame[i] = (uint8_t)(i * 7u + 1u);

    if (radio_send(frame, len, air_us) != 0)
        return -24;
    if (delay_us != NULL)
        *delay_us = micros() - t_beacon;
    return 0;
}

/* The accept path against the hub's published frame, before any of it goes near
 * a radio. This is the first frame the device will ever open, its body is 19
 * bytes - not a multiple of four - and it carries the key the whole network
 * hops on, so a length or offset error here costs the channel plan rather than
 * one frame.
 *
 * The three cleartext frames are covered too, now that the hub emits them as C:
 * the request and confirmation this side *builds* are compared against the
 * hub's bytes, and the response is parsed from the hub's bytes with the real
 * two-term ECDH on the PKA. A builder checked against a frame this side
 * assembled agrees with its own mistakes - which is how the grant length got
 * past me. */
int join_selftest(join_selftest_t *r) {
    join_result_t res;
    exchange_keys_t keys;
    uint8_t bad[sizeof(PV_FRAME_ACCEPT)];

    memset(r, 0, sizeof(*r));
    memset(&res, 0, sizeof(res));
    memset(&keys, 0, sizeof(keys));
    memcpy(keys.session, PV_KEY_SESSION, sizeof(keys.session));
    memcpy(keys.confirm_hub, PV_CONFIRM_HUB, sizeof(keys.confirm_hub));
    res.dev_id_be = 0x0000002Au;

    /* Set where the live path sets it, so the test reads the same source. */
    res.superframe = PAIR_REQ_SUPERFRAME;
    join_stats_t before = stats;

    r->accept_rc = handle_accept(&res, PV_FRAME_ACCEPT, sizeof(PV_FRAME_ACCEPT),
                                 &keys);
    r->accept_ok = r->accept_rc == 0;
    r->grant_ok  = res.slot == 0u && res.report_every == RADIO_REPORT_EVERY_DEFAULT;
    r->hop_key_ok = memcmp(res.hop_key, PV_NET_HOP_KEY, sizeof(PV_NET_HOP_KEY)) == 0;

    /* One flipped tag byte must be refused, and refused as a tag failure rather
     * than as a malformed frame - the two counters are what tell an operator
     * whether the air or the key is wrong. */
    memcpy(bad, PV_FRAME_ACCEPT, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0x01u;
    r->forged_rejected = handle_accept(&res, bad, sizeof(bad), &keys) == -10;

    /* And a frame from outside the quiesce, refused before the tag is spent. */
    memcpy(bad, PV_FRAME_ACCEPT, sizeof(bad));
    bad[offsetof(radio_pair_accept_t, superframe) + 3] += 1u;   /* +2^24 superframes */
    r->stale_rejected = handle_accept(&res, bad, sizeof(bad), &keys) == -9;

    r->confirm_ok = exchange_confirm_equal(PV_CONFIRM_HUB, keys.confirm_hub) == 1;

    /* The published identity, so the two scalar multiplications run for real
     * and Z is reproduced rather than supplied. */
    pairing_ctx_t vp;
    memset(&vp, 0, sizeof(vp));
    memcpy(vp.priv, V_DEV_PRIV, sizeof(vp.priv));
    if (crypto_p256_public_from_private(vp.priv, vp.pub) == 0) {
        vp.have_key = 1;
        vp.hub_id = 0x33442211u;
        vp.dev_id = 0x0000002Au;
        vp.net_id = 0x0001u;
        memcpy(vp.dev_nonce, PV_DEV_NONCE, sizeof(vp.dev_nonce));

        uint8_t built[sizeof(PV_FRAME_REQ)];
        build_request(&vp, PAIR_REQ_SUPERFRAME, built);
        r->req_built_ok = memcmp(built, PV_FRAME_REQ, sizeof(built)) == 0;

        join_result_t vres;
        exchange_keys_t vkeys;
        memset(&vres, 0, sizeof(vres));
        memcpy(vres.hub_static, V_HUB_PUB_C, sizeof(vres.hub_static));
        /* The whole response path on the hub's own bytes: point validity, the
         * two ECDH, the transcript, the schedule, and its confirmation. */
        vres.superframe = PAIR_REQ_SUPERFRAME;
        r->rsp_parsed_ok = handle_response(&vp, &vres, PV_FRAME_RSP,
                                           sizeof(PV_FRAME_RSP), &vkeys) == 0;
        if (r->rsp_parsed_ok) {
            uint8_t conf[sizeof(PV_FRAME_CONF)];
            build_confirm(&vp, vkeys.confirm_dev, conf);
            r->conf_built_ok = memcmp(conf, PV_FRAME_CONF, sizeof(conf)) == 0;
        } else {
            r->conf_built_ok = 2u;      /* not reached, which is not a failure */
        }
        /* A response carrying the hub's static key as its ephemeral, refused
         * for that one reason and not because the confirmation fails. */
        uint8_t swapped[sizeof(PV_FRAME_RSP)];
        memcpy(swapped, PV_FRAME_RSP, sizeof(swapped));
        memcpy(swapped + offsetof(radio_pair_rsp_t, eph_pubkey), V_HUB_PUB_C, 33);
        r->eph_static_rejected =
            handle_response(&vp, &vres, swapped, sizeof(swapped), &vkeys) == -5;
        memset(&vkeys, 0, sizeof(vkeys));
        memset(&vp, 0, sizeof(vp));
    }

    memcpy(r->nonce, last_accept_nonce, sizeof(r->nonce));
    r->aad_len = RADIO_PAIR_ACCEPT_AAD_LEN;
    r->ct_len = last_accept_ct_len;
    stats = before;   /* a self-test must not move the counters an operator reads */
    return 0;
}
