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
static uint8_t z1_cache[32];
static uint8_t z1_valid;
static uint32_t last_accept_ms;
static uint8_t  ever_accepted;

static int derive_z1(const pair_init_ctx_t *ctx) {
    uint8_t peer[65];

    if (z1_valid)
        return 0;
    /* ECDH takes the uncompressed point; the wire and the store carry the compressed one.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (crypto_p256_decompress(ctx->hub_static_c[0], ctx->hub_static_c + 1, peer) != 0)
        return -1;
    if (crypto_p256_ecdh(ctx->dev_priv, peer, z1_cache) != 0)
        return -1;
    z1_valid = 1;
    stats.z1_derivations++;
    return 0;
}

void pair_init_key_from_z1(const uint8_t z1[32], uint32_t hub_id, uint32_t dev_id,
                           uint8_t key_out[32]) {
    uint8_t salt[8];

    /* The seam: big-endian into the crypto, little-endian on the wire.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    salt[0] = (uint8_t)(hub_id >> 24); salt[1] = (uint8_t)(hub_id >> 16);
    salt[2] = (uint8_t)(hub_id >> 8);  salt[3] = (uint8_t)hub_id;
    salt[4] = (uint8_t)(dev_id >> 24); salt[5] = (uint8_t)(dev_id >> 16);
    salt[6] = (uint8_t)(dev_id >> 8);  salt[7] = (uint8_t)dev_id;

    hkdf_sha256(salt, sizeof(salt), z1, 32u,
                (const uint8_t *)"openhub/v3/init", 15u, key_out, 32u);
}

int pair_init_key(const pair_init_ctx_t *ctx, uint8_t key_out[32]) {
    if (derive_z1(ctx) != 0)
        return -1;
    pair_init_key_from_z1(z1_cache, ctx->hub_id, ctx->dev_id, key_out);
    return 0;
}

int pair_init_prepare(const pair_init_ctx_t *ctx) {
    return derive_z1(ctx);
}

void pair_init_test_seed_z1(const uint8_t z1[32]) {
    memcpy(z1_cache, z1, sizeof(z1_cache));
    z1_valid = 1;
}

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
                                uint32_t *superframe_out) {
    uint8_t key[32], mac[SHA256_LEN];
    const uint32_t hdr = sizeof(radio_pair_init_t) - RADIO_PAIR_INIT_MAC_LEN;

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
        return PAIR_INIT_WRONG_NET;
    }
    /* Before any key work: an invitation to another device must cost this one nothing.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (le32(frame + 8) != ctx->dev_id) {
        stats.not_addressed++;
        return PAIR_INIT_NOT_ADDRESSED;
    }
    if (ctx->hub_static_c == NULL ||
        (ctx->hub_static_c[0] != 0x02u && ctx->hub_static_c[0] != 0x03u))
        return PAIR_INIT_NO_HUB_KEY;

    /* Before the MAC: the MAC is not what is being rationed.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (ever_accepted && (uint32_t)(now_ms - last_accept_ms) < PAIR_INIT_MIN_GAP_MS) {
        stats.rate_limited++;
        return PAIR_INIT_RATE_LIMITED;
    }
    /* Under the frame's hub_id: adopting it and verifying under it are one act.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (derive_z1(ctx) != 0) {
        stats.derive_failed++;
        return PAIR_INIT_DERIVE_FAILED;
    }
    pair_init_key_from_z1(z1_cache, hub, ctx->dev_id, key);
    hmac_sha256(key, sizeof(key), frame, hdr, mac);
    if (!mac_equal(mac, frame + hdr, RADIO_PAIR_INIT_MAC_LEN)) {
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
    memset(z1_cache, 0, sizeof(z1_cache));
    z1_valid = 0;
    ever_accepted = 0;
}

const char *pair_init_rc_name(pair_init_rc_t rc) {
    switch (rc) {
    case PAIR_INIT_OK:            return "ok";
    case PAIR_INIT_BAD_FRAME:     return "type, version or length";
    case PAIR_INIT_WRONG_NET:     return "another network";
    case PAIR_INIT_NOT_ADDRESSED: return "addressed to another device";
    case PAIR_INIT_NO_HUB_KEY:    return "no hub static key provisioned";
    case PAIR_INIT_DERIVE_FAILED: return "Z1 derivation failed";
    case PAIR_INIT_BAD_MAC:       return "MAC";
    case PAIR_INIT_REPLAY:        return "superframe at or below the ceiling";
    case PAIR_INIT_RATE_LIMITED:  return "rate limited";
    default:                      return "?";
    }
}
