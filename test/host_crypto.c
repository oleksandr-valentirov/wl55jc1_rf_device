/**
 * @file host_crypto.c
 * @brief The crypto surface hublogic.c needs, on a PC, and no more of it.
 *
 * The curve is **real** - Monocypher is vendored in this tree and the host can
 * run it - because a confirmation both sides compute from a shared secret is
 * the thing the exchange turns on. What is *not* real here is the AEAD: the
 * WL55 seals through the HAL, there is no software AES in this tree, and
 * `crypto_gcm_seal` below is a marker rather than a cipher.
 *
 * That is a stated limit and not a gap: what the grant carries is pinned by
 * `test_link` against `link_v7`, and this suite is about **when** the grant is
 * sent, which is `RADIO_PAIR_CONF_REGION` and nothing to do with a key.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <string.h>

#include "crypto.h"
#include "monocypher.h"

/* Seeded, so a failing run can be re-run and read rather than re-rolled. */
static uint32_t rng_state = 0x2026u;

void host_crypto_seed(uint32_t seed) {
    rng_state = seed ? seed : 1u;
}

int crypto_rng_word(uint32_t *out) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    *out = rng_state;
    return 0;
}

int crypto_x25519_public_from_private(const uint8_t *priv, uint8_t *pub) {
    crypto_x25519_public_key(pub, priv);
    return 0;
}

/* The device's own clamping, copied because the keys must be the same shape. */
int crypto_x25519_keygen(uint8_t *priv, uint8_t *pub) {
    for (int i = 0; i < 8; i++) {
        uint32_t w = 0;

        if (crypto_rng_word(&w) != 0) {
            memset(priv, 0, 32);
            return -1;
        }
        priv[4 * i]     = (uint8_t)w;
        priv[4 * i + 1] = (uint8_t)(w >> 8);
        priv[4 * i + 2] = (uint8_t)(w >> 16);
        priv[4 * i + 3] = (uint8_t)(w >> 24);
    }
    priv[0]  = (uint8_t)(priv[0] & 248u);
    priv[31] = (uint8_t)((priv[31] & 127u) | 64u);
    return crypto_x25519_public_from_private(priv, pub);
}

int crypto_x25519_ecdh(const uint8_t *priv, const uint8_t *peer_pub, uint8_t *shared) {
    int all_zero = 1;

    crypto_x25519(shared, priv, peer_pub);
    for (int i = 0; i < 32; i++)
        if (shared[i] != 0u) { all_zero = 0; break; }
    return all_zero ? -2 : 0;
}

/* Not a cipher: the file comment says why, and test_link is where sealing lives. */
int crypto_gcm_seal(const uint8_t *key16, const uint8_t *nonce12,
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *pt, uint16_t len,
                    uint8_t *ct, uint8_t *tag) {
    (void)aad; (void)aad_len;
    for (uint16_t i = 0; i < len; i++)
        ct[i] = (uint8_t)(pt[i] ^ key16[i & 15u] ^ nonce12[i % 12u]);
    for (int i = 0; i < 16; i++)
        tag[i] = (uint8_t)(key16[i] ^ (uint8_t)len);
    return 0;
}
