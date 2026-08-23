/* A tripwire, not a proof: this path makes no curve call, and one added is loud.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int crypto_x25519_ecdh(const uint8_t *priv, const uint8_t *peer, uint8_t *shared);
int crypto_x25519_keygen(uint8_t *priv, uint8_t *pub);

int crypto_x25519_ecdh(const uint8_t *priv, const uint8_t *peer, uint8_t *shared) {
    (void)priv; (void)peer; (void)shared;
    fprintf(stderr, "FAIL: the curve was reached from the invitation path\n");
    abort();
}

int crypto_x25519_keygen(uint8_t *priv, uint8_t *pub) {
    (void)priv; (void)pub;
    fprintf(stderr, "FAIL: the curve was reached from the invitation path\n");
    abort();
}
