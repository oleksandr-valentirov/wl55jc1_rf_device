/* PAIR_INIT: the hub's addressed, authenticated invitation to pair.
 *
 * The frame it replaces - the cleartext join beacon - could be forged by anyone
 * with a transmitter, so a device could be made to spend a pairing exchange on a
 * bystander. This one is MACed under a key only the enrolled pair can derive.
 *
 * Two obligations live here rather than on the hub, because the hub cannot
 * observe either of them: a rate limit on how often an invitation is acted on,
 * and a durable ceiling that refuses a replayed one outright. ADR-0021 records
 * them as device-side for exactly that reason - an unrated device is
 * indistinguishable from a working one until a battery is flat. */
#include <string.h>

#include "pair_init.h"
#include "crypto.h"
#include "sha256.h"

/* One accepted invitation per interval. The cost being bounded is not this
 * frame's - it is the ephemeral keygen and scalar multiply of the exchange that
 * follows, which an attacker would otherwise buy for one 28-byte frame. */
#define PAIR_INIT_MIN_GAP_MS  10000u

static pair_init_stats_t stats;
/* Z1 is a function of two static keys and nothing else, so it is the same value
 * for the life of the enrolment. Deriving it per frame would put a 103 ms
 * scalar multiply behind an unauthenticated frame, which is the denial of
 * service the rate limit is there to prevent - and the rate limit runs after
 * it. Cached, and `z1_derivations` exists so "once" is measured, not assumed. */
static uint8_t z1_cache[32];
static uint8_t z1_valid;
static uint32_t last_accept_ms;
static uint8_t  ever_accepted;

static int derive_z1(const pair_init_ctx_t *ctx) {
    uint8_t peer[65];

    if (z1_valid)
        return 0;
    /* ECDH takes the uncompressed point; the wire and the store carry the
     * compressed one. Passing the 33-byte form straight in fails every time,
     * and the vector check could not see it: seeding Z1 is what lets the
     * published frame reach the live verifier, and it steps over exactly this. */
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

    /* Big-endian into the crypto, little-endian on the wire - the one seam in
     * this frame where the two conventions meet. */
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

/* Constant time: a MAC compared with memcmp leaks where it first differs, and
 * the attacker chooses the frame. */
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

    /* Type, then version, then length - a future layout is a clean rejection
     * rather than a misparse that happens to be the right size. */
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
    /* A device that has never heard the network cannot compare - and the frame
     * that would have taught it is the beacon this one replaces. It does not
     * need to: hub_id is inside the MAC's salt, so a forger claiming another
     * one derives a different K_init and fails below. The ids authenticate
     * themselves; only hub_static has to arrive out of band, which is the
     * operator's job at enrolment. Strict again the moment one is stored. */
    if (ctx->hub_id != 0u && (net != ctx->net_id || hub != ctx->hub_id)) {
        stats.wrong_net++;
        return PAIR_INIT_WRONG_NET;
    }
    /* Addressed, and checked before any key work: an invitation to another
     * device must cost this one nothing. */
    if (le32(frame + 8) != ctx->dev_id) {
        stats.not_addressed++;
        return PAIR_INIT_NOT_ADDRESSED;
    }
    if (ctx->hub_static_c == NULL ||
        (ctx->hub_static_c[0] != 0x02u && ctx->hub_static_c[0] != 0x03u))
        return PAIR_INIT_NO_HUB_KEY;

    /* Before the MAC, because the MAC is not what is being rationed - see the
     * gap constant. An unauthenticated frame must not reach the exchange. */
    if (ever_accepted && (uint32_t)(now_ms - last_accept_ms) < PAIR_INIT_MIN_GAP_MS) {
        stats.rate_limited++;
        return PAIR_INIT_RATE_LIMITED;
    }
    /* Under the frame's hub_id, which is the whole point: adopting it and
     * verifying under it are the same act. Z1 does not depend on it - only the
     * salt does - so the cached scalar multiply still stands. */
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

    /* Only now. A forged superframe that moved the ceiling would lock out every
     * genuine invitation behind it, turning a replay guard into the denial of
     * service it exists to prevent - the hub learned the same thing about its
     * uplink floor an hour ago. Strictly ahead: equal is the recording. */
    uint32_t sf = le32(frame + 12);
    /* Not gated on having accepted one this session. The ceiling is durable
     * precisely so it outlives a power cycle, and `ever_accepted` is RAM: with
     * it in this condition the guard did nothing at the one moment it exists
     * for - a reboot, which is what an attacker with a recording arranges. The
     * rate limit above is RAM-scoped by nature and keeps its flag. */
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
