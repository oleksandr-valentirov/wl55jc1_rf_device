#pragma once

#include <stdint.h>

#define SHA256_LEN 32
#define HKDF_MAX_INFO 64

/** @brief One-shot SHA-256 over a buffer; software, this part has no HASH block. */
void sha256(const uint8_t *data, uint32_t len, uint8_t *out);

/** @brief HMAC-SHA-256. */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *msg, uint32_t msg_len, uint8_t *out);

/** @brief HKDF-SHA-256, extract and expand in one call. */
void hkdf_sha256(const uint8_t *salt, uint32_t salt_len,
                 const uint8_t *ikm, uint32_t ikm_len,
                 const uint8_t *info, uint32_t info_len,
                 uint8_t *out, uint32_t out_len);
