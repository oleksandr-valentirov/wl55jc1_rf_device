/* Pairing: one P-256 ECDH over the air, then HKDF to the session and hop keys.
 *
 * The exchange is cleartext because neither end has a key yet. That is not a
 * weakness on its own - an attacker can start an exchange but cannot finish one,
 * because the hub checks the device's public key against a fingerprint the
 * operator supplied out of band. Without that check pairing is anonymous ECDH
 * and trivially relayed, which must not be mistaken for a working design. */
#include <stddef.h>
#include <string.h>

#include "pairing.h"

/* 45 bytes: 12 of header and one SEC1 compressed point. Written as the sum it
 * has to equal rather than as 45, so a change to either part is a build error
 * and not a frame the far side silently refuses on length. */
/* Against the contract's literal, not against sizeof - comparing the struct to
 * itself is the vacuous form, and it is what let a 45-byte local layout sit
 * beside a 49-byte shared one with both sides asserting their own number. */
_Static_assert(sizeof(radio_pair_req_t) == 57u, "PAIR_REQ must stay 57 bytes");
#include "radio.h"
#include "sha256.h"
#include "timebase.h"

static const uint8_t info_session[] = "openhub/v1/session";
static const uint8_t info_hop[]     = "openhub/v1/hop";

/* One place, because the fingerprint and the transmitted key must be a hash of
 * the same bytes - two copies of this can drift and the symptom is an enrolment
 * that never matches. */
static void compress_pub(const uint8_t *sec1_65, uint8_t *out33) {
    out33[0] = (uint8_t)(0x02u | (sec1_65[P256_PUB_LEN - 1] & 1u));
    memcpy(out33 + 1, sec1_65 + 1, P256_PUB_COMPRESSED_LEN - 1u);
}

int pairing_new_nonce(pairing_ctx_t *ctx) {
    memset(ctx->dev_nonce, 0, sizeof(ctx->dev_nonce));
    for (uint32_t i = 0; i < sizeof(ctx->dev_nonce); i += 4u) {
        uint32_t w;
        if (crypto_rng_word(&w) != 0)
            return -1;
        ctx->dev_nonce[i]     = (uint8_t)w;
        ctx->dev_nonce[i + 1] = (uint8_t)(w >> 8);
        ctx->dev_nonce[i + 2] = (uint8_t)(w >> 16);
        ctx->dev_nonce[i + 3] = (uint8_t)(w >> 24);
    }
    return 0;
}

/* All zero is what an RNG that never ran leaves behind. It is the unseeded
 * signature and not a quality test - a generator stuck on any other constant
 * walks straight through - so the hub's per-device memory of the last nonce is
 * the check that means something. This one only fires a round earlier. */
static int nonce_is_zero(const pairing_ctx_t *ctx) {
    uint8_t any = 0;
    for (uint32_t i = 0; i < sizeof(ctx->dev_nonce); i++)
        any |= ctx->dev_nonce[i];
    return any == 0;
}

int pairing_keygen(pairing_ctx_t *ctx) {
    if (crypto_p256_keygen(ctx->priv, ctx->pub) != 0)
        return -1;
    ctx->have_key = 1;
    return 0;
}

/* The compressed point, so the fingerprint and the wire always name the same
 * bytes. This hashed the 65-byte uncompressed point and truncated to 6, against
 * a hub that demands 64 hex digits over the 33-byte compressed one - so
 * enrolment could never have matched. pair_v2 pins the domain; found by diffing
 * against it rather than by reading either side. */
/* The compressed point itself, because v3's operator value is the key rather
 * than its hash: a curve point cannot be recovered from SHA-256, so a hub that
 * needs Z1 must be given the key. Sized twice, same as the fingerprint. */
uint8_t pairing_pubkey_c(const pairing_ctx_t *ctx, uint8_t out[P256_PUB_COMPRESSED_LEN],
                         uint32_t out_len) {
    if (!ctx->have_key || out_len < P256_PUB_COMPRESSED_LEN)
        return 0;
    compress_pub(ctx->pub, out);
    return P256_PUB_COMPRESSED_LEN;
}

uint8_t pairing_fingerprint(const pairing_ctx_t *ctx, uint8_t out[SHA256_LEN],
                            uint32_t out_len) {
    uint8_t compressed[P256_PUB_COMPRESSED_LEN];
    /* Bounded twice, and the two catch different callers. The array parameter
     * makes gcc refuse a short *array* at compile time - measured, -Wall alone
     * at -O0 - which a bare uint8_t* does not. out_len catches the caller that
     * passes a pointer, where no compiler can see the size. Widening this from
     * 6 to 32 bytes turned the one existing caller into a stack overflow with
     * neither check in place. */
    if (!ctx->have_key || out_len < SHA256_LEN)
        return 0;
    compress_pub(ctx->pub, compressed);
    sha256(compressed, sizeof(compressed), out);
    return SHA256_LEN;
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Both ids travel in both directions. Carrying only the sender's would let a
 * device pairing at the same time as another accept the wrong response and
 * derive a session against a hub that thinks it is talking to someone else -
 * safe in the keys, but presenting as correct ECDH with different keys. */
static void build_pair_frame(const pairing_ctx_t *ctx, uint8_t type, uint8_t *out) {
    radio_pair_req_t f;
    f.type       = type;
    f.version    = PAIR_VERSION;
    f.net_id     = ctx->net_id;             /* wire fields stay little-endian */
    f.hub_id     = ctx->hub_id;
    f.dev_id     = ctx->dev_id;
    f.superframe = ctx->superframe;
    memcpy(f.dev_nonce, ctx->dev_nonce, sizeof(f.dev_nonce));
    compress_pub(ctx->pub, f.pubkey);
    memcpy(out, &f, sizeof(f));
}

/* The SINGLE-TERM derivation from wire_v3, and no longer what pairing produces.
 *
 * pair_v2 derives from a 64-byte Z over two ECDH terms and salts with the
 * request's superframe and nonce; exchange.c is that. This stays because
 * wire_v3 still pins it as a primitive and the bench command below still runs
 * the one-frame exchange. Named for the construction rather than for the job,
 * so a green result here cannot be read as evidence about pairing. */
static void derive_single_term_v1(pairing_ctx_t *ctx, const uint8_t *shared_x) {
    uint8_t salt[8];
    salt[0] = (uint8_t)(ctx->hub_id >> 24);
    salt[1] = (uint8_t)(ctx->hub_id >> 16);
    salt[2] = (uint8_t)(ctx->hub_id >> 8);
    salt[3] = (uint8_t)ctx->hub_id;
    salt[4] = (uint8_t)(ctx->dev_id >> 24);
    salt[5] = (uint8_t)(ctx->dev_id >> 16);
    salt[6] = (uint8_t)(ctx->dev_id >> 8);
    salt[7] = (uint8_t)ctx->dev_id;

    hkdf_sha256(salt, sizeof(salt), shared_x, 32,
                info_session, sizeof(info_session) - 1u, ctx->session, 16);
    hkdf_sha256(salt, sizeof(salt), shared_x, 32,
                info_hop, sizeof(info_hop) - 1u, ctx->hop, 16);
    ctx->paired = 1;
}

static int exchange(pairing_ctx_t *ctx, uint8_t send_type, uint8_t expect_type,
                    uint32_t timeout_ms, int send_first) {
    uint8_t frame[PAIR_FRAME_LEN];
    uint8_t rx[PAIR_FRAME_LEN];
    uint8_t shared_x[32];
    radio_rx_info_t info = {0};

    if (!ctx->have_key && pairing_keygen(ctx) != 0)
        return -1;
    /* Fresh per attempt, and a refusal to transmit rather than a zero nonce:
     * sending one restores the replay this field exists to remove. */
    if (pairing_new_nonce(ctx) != 0 || nonce_is_zero(ctx))
        return -7;
    build_pair_frame(ctx, send_type, frame);

    if (send_first && radio_send(frame, sizeof(frame), NULL) != 0)
        return -1;

    info.timeout_us = timeout_ms * 1000u;
    if (radio_receive(rx, sizeof(rx), &info) != 0)
        return -3;
    if (info.len != PAIR_FRAME_LEN || rx[0] != expect_type || rx[1] != PAIR_VERSION)
        return -4;

    uint32_t rx_net = (uint32_t)rx[2] | ((uint32_t)rx[3] << 8);
    uint32_t rx_hub = get_le32(rx + 4);
    uint32_t rx_dev = get_le32(rx + 8);
    if (rx_net != ctx->net_id)
        return -6;
    /* Filter before spending 103 ms of PKA on a frame that is not ours. The
     * device knows both ids by now; the hub is still learning dev_id. */
    if (expect_type == PAIR_TYPE_RESPONSE) {
        if (rx_dev != ctx->dev_id || rx_hub != ctx->hub_id)
            return -6;
    } else if (rx_hub != ctx->hub_id) {
        return -6;
    }

    if (expect_type == PAIR_TYPE_REQUEST) {
        ctx->dev_id = rx_dev;
        /* Rebuild: the reply must carry the id just learned, not a zero. */
        build_pair_frame(ctx, send_type, frame);
    }

    if (!send_first) {
        /* Answer before the peer gives up waiting; it is already listening. */
        if (radio_send(frame, sizeof(frame), NULL) != 0)
            return -1;
    }

    uint8_t peer_pub[P256_PUB_LEN];
    /* Offsets come from the shared struct, so a field added there moves both
     * ends together instead of only the one that noticed. */
    if (crypto_p256_decompress(rx[offsetof(radio_pair_req_t, pubkey)],
                               rx + offsetof(radio_pair_req_t, pubkey) + 1u,
                               peer_pub) != 0)
        return -5;

    int rc = crypto_p256_ecdh(ctx->priv, peer_pub, shared_x);
    if (rc != 0)
        return (rc == -2) ? -5 : -1;

    derive_single_term_v1(ctx, shared_x);
    memset(shared_x, 0, sizeof(shared_x));
    return 0;
}

/* The device must know which hub it is addressing before it asks, so that the
 * response filter above has something to compare against. */
int pairing_set_hub(pairing_ctx_t *ctx, uint32_t hub_id) {
    ctx->hub_id = hub_id;
    return 0;
}

/* The salt crosses the one boundary the wire format deliberately has two sides
 * of: the ids travel little-endian and enter HKDF big-endian. The vectors pin
 * it only for the literal salt bytes, so this runs the real derive() over ids
 * held as integers - which is what pairing does with ids taken off the air. */
int pairing_salt_check(const uint8_t *shared_x, uint32_t hub_id, uint32_t dev_id,
                       uint8_t *session_out, uint8_t *hop_out) {
    pairing_ctx_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.hub_id = hub_id;
    probe.dev_id = dev_id;
    derive_single_term_v1(&probe, shared_x);
    memcpy(session_out, probe.session, 16);
    memcpy(hop_out, probe.hop, 16);
    return 0;
}

int pairing_run_device(pairing_ctx_t *ctx, uint32_t timeout_ms) {
    return exchange(ctx, PAIR_TYPE_REQUEST, PAIR_TYPE_RESPONSE, timeout_ms, 1);
}

int pairing_run_hub(pairing_ctx_t *ctx, uint32_t timeout_ms) {
    return exchange(ctx, PAIR_TYPE_RESPONSE, PAIR_TYPE_REQUEST, timeout_ms, 0);
}
