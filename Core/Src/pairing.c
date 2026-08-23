/* Two X25519 over the air, then HKDF. Cleartext; the fingerprint is what binds it.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#include <stddef.h>
#include <string.h>

#include "pairing.h"

/* The sum it must equal, against the contract's literal rather than against sizeof.
 * radio_devices_docs/wl55_device/radio/pairing.md */
_Static_assert(sizeof(radio_pair_req_t) == 56u, "PAIR_REQ must stay 56 bytes");
#include "radio.h"
#include "sha256.h"

static const uint8_t info_session[] = "openhub/v1/session";
static const uint8_t info_hop[]     = "openhub/v1/hop";

/* One place: the fingerprint and the transmitted key must hash the same bytes.
 * radio_devices_docs/wl55_device/radio/pairing.md */

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

/* The unseeded signature, not a quality test: any other constant walks through.
 * radio_devices_docs/wl55_device/radio/pairing.md */
static int nonce_is_zero(const pairing_ctx_t *ctx) {
    uint8_t any = 0;
    for (uint32_t i = 0; i < sizeof(ctx->dev_nonce); i++)
        any |= ctx->dev_nonce[i];
    return any == 0;
}

int pairing_keygen(pairing_ctx_t *ctx) {
    if (crypto_x25519_keygen(ctx->priv, ctx->pub) != 0)
        return -1;
    ctx->have_key = 1;
    return 0;
}

/* The compressed point, so the fingerprint and the wire name the same bytes.
 * radio_devices_docs/wl55_device/radio/pairing.md */
uint8_t pairing_pubkey_c(const pairing_ctx_t *ctx, uint8_t out[X25519_PUB_LEN],
                         uint32_t out_len) {
    if (!ctx->have_key || out_len < X25519_PUB_LEN)
        return 0;
    memcpy(out, ctx->pub, X25519_PUB_LEN);
    return X25519_PUB_LEN;
}

uint8_t pairing_fingerprint(const pairing_ctx_t *ctx, uint8_t out[SHA256_LEN],
                            uint32_t out_len) {
    /* Bounded twice: the array parameter catches a short array, out_len a bare pointer.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (!ctx->have_key || out_len < SHA256_LEN)
        return 0;
    /* The member's width, never a local pointer's: sizeof on one hashed four bytes.
     * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
    sha256(ctx->pub, sizeof(ctx->pub), out);
    return SHA256_LEN;
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Both ids both ways: one id lets two devices derive against the wrong peer.
 * radio_devices_docs/wl55_device/radio/pairing.md */
static void build_pair_frame(const pairing_ctx_t *ctx, uint8_t type, uint8_t *out) {
    radio_pair_req_t f;
    f.type       = type;
    f.version    = PAIR_VERSION;
    f.net_id     = ctx->net_id;             /* wire fields stay little-endian */
    f.hub_id     = ctx->hub_id;
    f.dev_id     = ctx->dev_id;
    f.superframe = ctx->superframe;
    memcpy(f.dev_nonce, ctx->dev_nonce, sizeof(f.dev_nonce));
    memcpy(f.pubkey, ctx->pub, sizeof(f.pubkey));
    memcpy(out, &f, sizeof(f));
}

/* The SINGLE-TERM derivation from wire_v4, named for the construction not the job.
 * radio_devices_docs/wl55_device/radio/pairing.md */
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
    /* Fresh per attempt, refused rather than sent as zero: zero restores the replay.
     * radio_devices_docs/wl55_device/radio/pairing.md */
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
    /* Filter before 103 ms of PKA. The device knows both ids; the hub is still learning.
     * radio_devices_docs/wl55_device/radio/pairing.md */
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


    /* Offsets from the shared struct, so a new field moves both ends together.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    int rc = crypto_x25519_ecdh(ctx->priv,
                                rx + offsetof(radio_pair_req_t, pubkey), shared_x);
    if (rc != 0)
        return (rc == -2) ? -5 : -1;

    derive_single_term_v1(ctx, shared_x);
    memset(shared_x, 0, sizeof(shared_x));
    return 0;
}

/* Ids held as integers, which is what pairing does with ids taken off the air.
 * radio_devices_docs/wl55_device/radio/pairing.md */
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
