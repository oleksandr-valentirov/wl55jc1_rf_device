/* The out-of-band anchor: it must hash the whole key, and nothing has ever run it.
 * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
#include <stdio.h>
#include <string.h>

#include "pairing.h"

/* The five the module reaches for; none of them is on this path.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
int crypto_rng_word(uint32_t *out) { (void)out; return -1; }
int crypto_x25519_keygen(uint8_t *p, uint8_t *q) { (void)p; (void)q; return -1; }
int crypto_x25519_ecdh(const uint8_t *a, const uint8_t *b, uint8_t *c) {
    (void)a; (void)b; (void)c; return -1;
}
int radio_send(const uint8_t *p, uint8_t l, uint32_t *a) {
    (void)p; (void)l; (void)a; return -1;
}
int radio_receive(uint8_t *p, uint8_t m, void *i) { (void)p; (void)m; (void)i; return -1; }

/* SHA-256 of 00 01 02 ... 1f, taken outside this tree. */
static const uint8_t FP_OF_COUNTING_KEY[32] = {
    0x63,0x0d,0xcd,0x29,0x66,0xc4,0x33,0x66, 0x91,0x12,0x54,0x48,0xbb,0xb2,0x5b,0x4f,
    0xf4,0x12,0xa4,0x9c,0x73,0x2d,0xb2,0xc8, 0xab,0xc1,0xb8,0x58,0x1b,0xd7,0x10,0xdd
};

static int fails;

static void check(const char *name, int ok) {
    printf("  %-46s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void fill(pairing_ctx_t *c, const uint8_t *pub) {
    memset(c, 0, sizeof(*c));
    memcpy(c->pub, pub, X25519_PUB_LEN);
    c->have_key = 1;
}

int main(void) {
    pairing_ctx_t a, b;
    uint8_t key[X25519_PUB_LEN], alt[X25519_PUB_LEN];
    uint8_t fa[SHA256_LEN], fb[SHA256_LEN];

    printf("== test_fingerprint\n");

    for (int i = 0; i < X25519_PUB_LEN; i++)
        key[i] = (uint8_t)i;
    fill(&a, key);
    check("returns the full digest length",
          pairing_fingerprint(&a, fa, sizeof(fa)) == SHA256_LEN);
    check("hashes all 32 bytes of the key",
          memcmp(fa, FP_OF_COUNTING_KEY, SHA256_LEN) == 0);

    /* Every byte, not a sample: a pointer's sizeof is 4 on the target and 8 here.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    {
        int reached = 0;

        for (int i = 0; i < X25519_PUB_LEN; i++) {
            memcpy(alt, key, sizeof(alt));
            alt[i] ^= 0xFFu;
            fill(&b, alt);
            pairing_fingerprint(&b, fb, sizeof(fb));
            if (memcmp(fa, fb, SHA256_LEN) != 0)
                reached++;
        }
        check("every one of the 32 key bytes reaches the digest",
              reached == X25519_PUB_LEN);
    }

    fill(&a, key);
    a.have_key = 0;
    check("refuses a context with no key", pairing_fingerprint(&a, fa, sizeof(fa)) == 0);

    fill(&a, key);
    check("refuses an output shorter than the digest",
          pairing_fingerprint(&a, fa, SHA256_LEN - 1u) == 0);

    printf(fails ? "\n%d fingerprint check(s) FAILED\n" : "\nall fingerprint checks passed\n",
           fails);
    return fails ? 1 : 0;
}
