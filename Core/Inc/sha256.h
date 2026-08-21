#pragma once

#include <stdint.h>

#define SHA256_LEN 32
#define HKDF_MAX_INFO 64

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t fill;
} sha256_ctx_t;

/** @brief Starts a SHA-256 context; software, this part has no HASH block. */
void sha256_init(sha256_ctx_t *c);

/** @brief Absorbs a chunk into a SHA-256 context. */
void sha256_update(sha256_ctx_t *c, const uint8_t *data, uint32_t len);

/** @brief Finishes a SHA-256 context and writes the 32-byte digest. */
void sha256_final(sha256_ctx_t *c, uint8_t *out);

/** @brief One-shot SHA-256 over a buffer. */
void sha256(const uint8_t *data, uint32_t len, uint8_t *out);

/** @brief HMAC-SHA-256. */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *msg, uint32_t msg_len, uint8_t *out);

/** @brief HKDF-SHA-256, extract and expand in one call. */
void hkdf_sha256(const uint8_t *salt, uint32_t salt_len,
                 const uint8_t *ikm, uint32_t ikm_len,
                 const uint8_t *info, uint32_t info_len,
                 uint8_t *out, uint32_t out_len);
