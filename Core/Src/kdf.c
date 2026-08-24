/* This side's supply of the key schedule's two hash operations, over sha256.c.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#include "kdf.h"
#include "sha256.h"

/* Too long an info leaves the software HKDF's output stale; the seam says so.
 * radio_devices_docs/wl55_device/security/self-tests.md */
int crypto_hkdf_sha256(const uint8_t *salt, uint32_t salt_len,
                       const uint8_t *ikm, uint32_t ikm_len,
                       const uint8_t *info, uint32_t info_len,
                       uint8_t *out, uint32_t out_len) {
    if (info_len > HKDF_MAX_INFO)
        return -1;
    hkdf_sha256(salt, salt_len, ikm, ikm_len, info, info_len, out, out_len);
    return 0;
}

int crypto_hmac_sha256(const uint8_t *key, uint32_t key_len,
                       const uint8_t *msg, uint32_t msg_len,
                       uint8_t out[CRYPTO_SHA256_LEN]) {
    hmac_sha256(key, key_len, msg, msg_len, out);
    return 0;
}

/* One width, checked where both names are visible; a disagreement truncates. */
_Static_assert(SHA256_LEN == (int)CRYPTO_SHA256_LEN,
               "the seam's hash width is not this side's hash width");
