/* The curve, stubbed to abort: reaching it means Z1 was not seeded.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int crypto_p256_decompress(uint8_t prefix, const uint8_t *x, uint8_t *out);
int crypto_p256_ecdh(const uint8_t *priv, const uint8_t *peer, uint8_t *shared);

int crypto_p256_decompress(uint8_t prefix, const uint8_t *x, uint8_t *out) {
    (void)prefix; (void)x; (void)out;
    fprintf(stderr, "FAIL: the curve was reached; Z1 was not seeded\n");
    abort();
}

int crypto_p256_ecdh(const uint8_t *priv, const uint8_t *peer, uint8_t *shared) {
    (void)priv; (void)peer; (void)shared;
    fprintf(stderr, "FAIL: the curve was reached; Z1 was not seeded\n");
    abort();
}
