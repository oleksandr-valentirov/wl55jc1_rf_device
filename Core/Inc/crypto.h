#pragma once

#include <stdint.h>

typedef struct {
    uint8_t  ct_ok;
    uint8_t  tag_ok;
    uint8_t  pt_ok;
    uint8_t  dec_tag_ok;
    uint32_t encrypt_us;
    uint32_t decrypt_us;
} crypto_kat_result_t;

typedef struct {
    uint8_t  point_ok;
    uint8_t  shared_ok;
    uint8_t  reject_ok;
    uint32_t mul_us;
    uint32_t ecdh_us;
} crypto_x25519_result_t;

/** @brief Runs the AES-GCM known-answer test. */
int crypto_gcm_kat(crypto_kat_result_t *r);

/** @brief Runs the X25519 known-answer test against RFC 7748. */
int crypto_x25519_kat(crypto_x25519_result_t *r);

#define CRYPTO_GCM_MAX_AAD  16
#define CRYPTO_GCM_MAX_LEN  64

/** @brief Seals a payload under a key and a 12-byte nonce. */
int crypto_gcm_seal(const uint8_t *key16, const uint8_t *nonce12,
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *pt, uint16_t len,
                    uint8_t *ct, uint8_t *tag);
/** @brief Opens a sealed payload; -2 is a tag failure and clears the output. */
int crypto_gcm_open(const uint8_t *key16, const uint8_t *nonce12,
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *ct, uint16_t len,
                    uint8_t *pt, const uint8_t *tag);

/** @brief Raw AES-128-ECB, one block; the hop PRF needs a cipher, not an AEAD. */
int crypto_aes_ecb_block(const uint8_t *key16, const uint8_t in[16], uint8_t out[16]);

/** @brief Probes the accelerator's datatype handling instead of trusting it. */
int crypto_aes_ecb_datatype_probe(const uint8_t *key16, const uint8_t in[16],
                                  uint8_t out_8b[16], uint8_t out_32b[16]);

/* One width and one representation: RFC 7748 has no second form. ADR-0025 */
#define X25519_PRIV_LEN  32
#define X25519_PUB_LEN   32

/** @brief A checked TRNG word; zeroes the output on failure, never leaks a stale one. */
int crypto_rng_word(uint32_t *out);
/** @brief 0 when the TRNG reports no seed or clock error; *sr is the raw status. */
int crypto_rng_health(uint32_t *sr);

/** @brief Draws a clamped X25519 keypair from the TRNG. */
int crypto_x25519_keygen(uint8_t *priv, uint8_t *pub);

/** @brief Recomputes the public key from a stored private one. */
int crypto_x25519_public_from_private(const uint8_t *priv, uint8_t *pub);
/** @brief ECDH; -2 rejects the all-zero secret a low-order point produces. */
int crypto_x25519_ecdh(const uint8_t *priv, const uint8_t *peer_pub, uint8_t *shared);

typedef struct {
    uint8_t  point_valid;
    uint8_t  ecdh_ok;
    uint8_t  session_ok;
    uint8_t  hop_ok;
    uint8_t  ratchet_ok;
    uint8_t  cipher_ok;
    uint8_t  tag_ok;
    uint8_t  open_ok;
    uint8_t  forge_rejected;
    uint8_t  odd_seal_ok;
    uint8_t  odd_open_ok;
    uint8_t  shared_reject_ok;
    uint32_t total_us;
    const char *digest;
} crypto_wire_result_t;

/** @brief Checks this build against the hub's published wire vectors. */
int crypto_wire_kat(crypto_wire_result_t *r);

typedef struct {
    uint8_t  salt_ok;
    uint8_t  transcript_ok;
    uint8_t  session_ok;
    uint8_t  confirm_hub_ok;
    uint8_t  confirm_dev_ok;
    uint8_t  accept_open_ok;        /* 19 bytes: the partial-final-word case */
    uint8_t  hop_key_ok;
    uint8_t  accept_forge_rejected;
    uint8_t  uplink_seal_ok;
    uint8_t  eph_static_rejected;
    uint8_t  after_misconfig_ok;    /* CRYP left hostile by another user */
    uint8_t  fips_ok;               /* FIPS-197 C.1, from neither side */
    uint32_t total_us;
    const char *digest;
} crypto_pair_result_t;

/** @brief Checks this build against the hub's published pairing vectors. */
int crypto_pair_kat(crypto_pair_result_t *r);
