/* PAIR_INIT: the hub's addressed, MACed invitation, and two guards it cannot see.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#include <string.h>

#include "pair_init.h"
#include "crypto.h"
#include "sha256.h"

/* One per interval. The bounded cost is the exchange behind it, not this frame.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#define PAIR_INIT_MIN_GAP_MS  10000u

static pair_init_stats_t stats;
/* Cached: Z1 per frame puts 103 ms behind an unauthenticated frame.
 * radio_devices_docs/wl55_device/radio/pairing.md */
static uint32_t last_accept_ms;
static uint8_t  ever_accepted;

/* Constant time: memcmp leaks where it differs and the attacker picks the frame.
 * radio_devices_docs/wl55_device/radio/pairing.md */
static int mac_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++)
        d = (uint8_t)(d | (a[i] ^ b[i]));
    return d == 0;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

pair_init_rc_t pair_init_verify(const pair_init_ctx_t *ctx, const uint8_t *frame,
                                uint8_t len, uint32_t now_ms, uint32_t ceiling,
                                uint32_t *superframe_out, uint8_t *hub_static_out) {
    static const uint8_t zero_mac[RADIO_PAIR_INIT_MAC_LEN] = {0};
    const uint32_t hdr = sizeof(radio_pair_init_t) - RADIO_PAIR_INIT_MAC_LEN;

    (void)hdr;

    /* Type, version, length: a future layout is a rejection, not a lucky misparse.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (len < 2u || frame[0] != RADIO_FRAME_PAIR_INIT) {
        stats.bad_frame++;
        return PAIR_INIT_BAD_FRAME;
    }
    stats.seen++;
    if (frame[1] != RADIO_PAIR_INIT_VERSION || len != sizeof(radio_pair_init_t)) {
        stats.bad_frame++;
        return PAIR_INIT_BAD_FRAME;
    }

    uint16_t net = (uint16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8));
    uint32_t hub = le32(frame + 4);
    /* hub_id is inside the salt, so the ids authenticate themselves. Strict once stored.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (ctx->hub_id != 0u && (net != ctx->net_id || hub != ctx->hub_id)) {
        stats.wrong_net++;
        /* Both sides, or the console cannot say which hub it was. */
        stats.wrong_net_hub      = hub;
        stats.wrong_net_net      = net;
        stats.wrong_net_want     = ctx->hub_id;
        stats.wrong_net_want_net = ctx->net_id;
        return PAIR_INIT_WRONG_NET;
    }
    /* Before any key work: an invitation to another device must cost this one nothing.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (le32(frame + 8) != ctx->dev_id) {
        stats.not_addressed++;
        return PAIR_INIT_NOT_ADDRESSED;
    }
    /* Before the MAC: the MAC is not what is being rationed.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (ever_accepted && (uint32_t)(now_ms - last_accept_ms) < PAIR_INIT_MIN_GAP_MS) {
        stats.rate_limited++;
        return PAIR_INIT_RATE_LIMITED;
    }
    /* The mode is the device's own, never the frame's claim.
     * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
    if (frame[16] != ctx->enrol_mode) {
        stats.bad_mode++;
        return PAIR_INIT_BAD_MODE;
    }
    /* Mode OPEN carries the field and no MAC in it; anything else is forgery. */
    if (!mac_equal(frame + hdr, zero_mac, RADIO_PAIR_INIT_MAC_LEN)) {
        stats.bad_mac++;
        return PAIR_INIT_BAD_MAC;
    }

    /* Only now, and strictly ahead: equal is the recording, and early locks everyone out.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    uint32_t sf = le32(frame + 12);
    /* Not gated on ever_accepted: that is RAM, and the reboot is the moment this exists for.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if ((int32_t)(sf - ceiling) <= 0) {
        stats.replay++;
        return PAIR_INIT_REPLAY;
    }

    /* The one thing this frame is trusted for, and only after every check. */
    if (hub_static_out != NULL)
        memcpy(hub_static_out, frame + 17, 32);

    stats.accepted++;
    stats.last_superframe = sf;
    stats.ceiling = sf;
    last_accept_ms = now_ms;
    ever_accepted = 1;
    if (superframe_out != NULL)
        *superframe_out = sf;
    return PAIR_INIT_OK;
}

void pair_init_stats(pair_init_stats_t *out) { *out = stats; }

void pair_init_stats_reset(void) {
    memset(&stats, 0, sizeof(stats));
}

void pair_init_forget(void) {
    ever_accepted = 0;
}

const char *pair_init_rc_name(pair_init_rc_t rc) {
    switch (rc) {
    case PAIR_INIT_OK:            return "ok";
    case PAIR_INIT_BAD_FRAME:     return "type, version or length";
    case PAIR_INIT_WRONG_NET:     return "another network";
    case PAIR_INIT_NOT_ADDRESSED: return "addressed to another device";
    case PAIR_INIT_BAD_MODE:      return "enrolment mode is not this device's";
    case PAIR_INIT_BAD_MAC:       return "a MAC where mode OPEN carries none";
    case PAIR_INIT_REPLAY:        return "superframe at or below the ceiling";
    case PAIR_INIT_RATE_LIMITED:  return "rate limited";
    default:                      return "?";
    }
}
