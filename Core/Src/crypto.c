/* Known-answer tests for the WL55's crypto silicon, against host vectors not the HAL.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#include <string.h>

#include "main.h"
#include "crypto.h"
#include "load.h"
#include "crypto_vectors.h"
#include "timebase.h"

extern CRYP_HandleTypeDef hcryp;
extern PKA_HandleTypeDef  hpka;
extern RNG_HandleTypeDef  hrng;

#define GCM_PT_LEN      (sizeof(vec_gcm_pt))
#define GCM_BUF_WORDS   ((GCM_PT_LEN + 3u) / 4u)

/* Charged apart from AES: PKA time is time a scheduler could hand away.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#define PKA_TIMED(name, hal, argtype)                                          \
    static HAL_StatusTypeDef name(PKA_HandleTypeDef *h, argtype *a, uint32_t t) { \
        load_enter(LOAD_PKA);                                                  \
        HAL_StatusTypeDef st = hal(h, a, t);                                   \
        load_exit();                                                           \
        return st;                                                             \
    }

PKA_TIMED(pka_eccmul_timed, HAL_PKA_ECCMul, PKA_ECCMulInTypeDef)
PKA_TIMED(pka_pointcheck_timed, HAL_PKA_PointCheck, PKA_PointCheckInTypeDef)
PKA_TIMED(pka_modexp_timed, HAL_PKA_ModExp, PKA_ModExpInTypeDef)

/* Packed explicitly: KEYRx/IVRx are not subject to the DATATYPE swap.
 * radio_devices_docs/wl55_device/security/README.md */
static void pack_be(const uint8_t *src, uint32_t *dst, uint32_t words) {
    for (uint32_t i = 0; i < words; i++)
        dst[i] = ((uint32_t)src[4 * i] << 24) | ((uint32_t)src[4 * i + 1] << 16) |
                 ((uint32_t)src[4 * i + 2] << 8) | (uint32_t)src[4 * i + 3];
}

static int gcm_configure(uint32_t *key, uint32_t *iv, uint32_t *aad, uint32_t aad_len) {
    hcryp.Init.DataType        = CRYP_DATATYPE_8B;
    hcryp.Init.KeySize         = CRYP_KEYSIZE_128B;
    hcryp.Init.pKey            = key;
    hcryp.Init.pInitVect       = iv;
    hcryp.Init.Algorithm       = CRYP_AES_GCM_GMAC;
    hcryp.Init.Header          = aad;
    hcryp.Init.HeaderSize      = aad_len;
    hcryp.Init.DataWidthUnit   = CRYP_DATAWIDTHUNIT_BYTE;
    hcryp.Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_BYTE;
    hcryp.Init.KeyIVConfigSkip = CRYP_KEYIVCONFIG_ALWAYS;
    return (HAL_CRYP_Init(&hcryp) == HAL_OK) ? 0 : -1;
}

/* DATATYPE_8B: a 32-bit datatype reverses each group of four and still looks like AES.
 * radio_devices_docs/wl55_device/security/self-tests.md */
static int aes_ecb_block_inner(const uint8_t *key16, const uint8_t in[16], uint8_t out[16]) {
    uint32_t key[4];

    if (key16 == NULL || in == NULL || out == NULL)
        return -1;
    pack_be(key16, key, 4);

    hcryp.Init.DataType        = CRYP_DATATYPE_8B;
    hcryp.Init.KeySize         = CRYP_KEYSIZE_128B;
    hcryp.Init.pKey            = key;
    hcryp.Init.pInitVect       = NULL;
    hcryp.Init.Algorithm       = CRYP_AES_ECB;
    hcryp.Init.Header          = NULL;
    hcryp.Init.HeaderSize      = 0;
    hcryp.Init.DataWidthUnit   = CRYP_DATAWIDTHUNIT_BYTE;
    hcryp.Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_BYTE;
    hcryp.Init.KeyIVConfigSkip = CRYP_KEYIVCONFIG_ALWAYS;
    if (HAL_CRYP_Init(&hcryp) != HAL_OK)
        return -1;

    return (HAL_CRYP_Encrypt(&hcryp, (uint32_t *)(void *)in, 16,
                             (uint32_t *)(void *)out, 1000) == HAL_OK) ? 0 : -1;
}

int crypto_aes_ecb_block(const uint8_t *key16, const uint8_t in[16], uint8_t out[16]) {
    load_enter(LOAD_CRYPTO);
    int rc = aes_ecb_block_inner(key16, in, out);
    load_exit();
    return rc;
}

/* The same block under both datatypes, measured on silicon rather than argued.
 * radio_devices_docs/wl55_device/security/self-tests.md */
int crypto_aes_ecb_datatype_probe(const uint8_t *key16, const uint8_t in[16],
                                  uint8_t out_8b[16], uint8_t out_32b[16]) {
    uint32_t key[4];

    if (crypto_aes_ecb_block(key16, in, out_8b) != 0)
        return -1;

    pack_be(key16, key, 4);
    hcryp.Init.DataType = CRYP_DATATYPE_32B;
    hcryp.Init.pKey     = key;
    if (HAL_CRYP_Init(&hcryp) != HAL_OK)
        return -1;
    return (HAL_CRYP_Encrypt(&hcryp, (uint32_t *)(void *)in, 16,
                             (uint32_t *)(void *)out_32b, 1000) == HAL_OK) ? 0 : -1;
}

int crypto_gcm_kat(crypto_kat_result_t *r) {
    uint32_t key[4], iv[4], aad[(sizeof(vec_gcm_aad) + 3) / 4];
    uint32_t in[GCM_BUF_WORDS], out[GCM_BUF_WORDS], tag[4];

    memset(r, 0, sizeof(*r));
    pack_be(vec_gcm_key, key, 4);
    pack_be(vec_gcm_iv, iv, 3);
    /* J0 for a 12-byte IV is IV || 1, and the peripheral wants it already stepped.
     * radio_devices_docs/wl55_device/security/README.md */
    iv[3] = 0x00000002u;
    memcpy(aad, vec_gcm_aad, sizeof(vec_gcm_aad));

    if (gcm_configure(key, iv, aad, sizeof(vec_gcm_aad)) != 0)
        return -1;

    memcpy(in, vec_gcm_pt, GCM_PT_LEN);
    uint32_t t0 = micros();
    if (HAL_CRYP_Encrypt(&hcryp, in, (uint16_t)GCM_PT_LEN, out, 1000) != HAL_OK)
        return -1;
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&hcryp, tag, 1000) != HAL_OK)
        return -1;
    r->encrypt_us = micros() - t0;

    r->ct_ok  = (memcmp(out, vec_gcm_ct, GCM_PT_LEN) == 0);
    r->tag_ok = (memcmp(tag, vec_gcm_tag, sizeof(vec_gcm_tag)) == 0);

    if (gcm_configure(key, iv, aad, sizeof(vec_gcm_aad)) != 0)
        return -1;
    memcpy(in, vec_gcm_ct, GCM_PT_LEN);
    t0 = micros();
    if (HAL_CRYP_Decrypt(&hcryp, in, (uint16_t)GCM_PT_LEN, out, 1000) != HAL_OK)
        return -1;
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&hcryp, tag, 1000) != HAL_OK)
        return -1;
    r->decrypt_us = micros() - t0;

    r->pt_ok      = (memcmp(out, vec_gcm_pt, GCM_PT_LEN) == 0);
    r->dec_tag_ok = (memcmp(tag, vec_gcm_tag, sizeof(vec_gcm_tag)) == 0);
    return 0;
}

int crypto_p256_kat(crypto_p256_result_t *r) {
    PKA_ECCMulInTypeDef mul = {0};
    PKA_PointCheckInTypeDef chk = {0};
    PKA_ECCMulOutTypeDef out = {0};
    uint8_t qx[32], qy[32], bad_qy[32];

    memset(r, 0, sizeof(*r));

    mul.scalarMulSize = sizeof(vec_p256_d);
    mul.modulusSize   = sizeof(p256_prime);
    mul.coefSign      = 1;              /* a = -3, so |a| = 3 with a negative sign */
    mul.coefA         = p256_abs_a;
    mul.modulus       = p256_prime;
    mul.pointX        = p256_gx;
    mul.pointY        = p256_gy;
    mul.scalarMul     = vec_p256_d;

    uint32_t t0 = micros();
    if (pka_eccmul_timed(&hpka, &mul, 5000) != HAL_OK)
        return -1;
    r->mul_us = micros() - t0;
    out.ptX = qx;
    out.ptY = qy;
    HAL_PKA_ECCMul_GetResult(&hpka, &out);
    r->point_ok = (memcmp(qx, vec_p256_qx, sizeof(qx)) == 0) &&
                  (memcmp(qy, vec_p256_qy, sizeof(qy)) == 0);

    chk.modulusSize = sizeof(p256_prime);
    chk.coefSign    = 1;
    chk.coefA       = p256_abs_a;
    chk.coefB       = p256_b;
    chk.modulus     = p256_prime;
    chk.pointX      = vec_p256_qx;
    chk.pointY      = vec_p256_qy;
    t0 = micros();
    if (pka_pointcheck_timed(&hpka, &chk, 5000) != HAL_OK)
        return -1;
    r->check_us  = micros() - t0;
    r->valid_ok  = (HAL_PKA_PointCheck_IsOnCurve(&hpka) == 1u);

    /* An off-curve point is what an invalid-curve attack looks like on the wire.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    memcpy(bad_qy, vec_p256_qy, sizeof(bad_qy));
    bad_qy[31] ^= 0x01u;
    chk.pointY = bad_qy;
    if (pka_pointcheck_timed(&hpka, &chk, 5000) != HAL_OK)
        return -1;
    r->reject_ok = (HAL_PKA_PointCheck_IsOnCurve(&hpka) == 0u);
    return 0;
}

/* Included from the hub's tree, never copied: both sides must check the same bytes.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#include "wire_v3.h"
#include "sha256.h"

/* Pinned: a bumped version arriving here should stop the compiler, not the radio.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#if WIRE_VECTORS_VERSION != 3
#error "wire vector set changed; re-check the device side against the new bytes"
#endif

/* One path both directions, so key, IV and header setup cannot drift apart.
 * radio_devices_docs/wl55_device/security/README.md */
static int gcm_run_inner(const uint8_t *key16, const uint8_t *nonce12,
                   const uint8_t *aad, uint32_t aad_len,
                   const uint8_t *in, uint16_t len, uint8_t *out, uint8_t *tag,
                   int decrypt) {
    uint32_t k[4], iv[4];
    uint32_t hdr[CRYPTO_GCM_MAX_AAD / 4];
    uint32_t ibuf[CRYPTO_GCM_MAX_LEN / 4], obuf[CRYPTO_GCM_MAX_LEN / 4], t[4];

    if (aad_len > CRYPTO_GCM_MAX_AAD || len > CRYPTO_GCM_MAX_LEN)
        return -1;
    pack_be(key16, k, 4);
    pack_be(nonce12, iv, 3);
    iv[3] = 0x00000002u;
    /* Zeroed before the copy: GHASH sees the final block and decrypt does not mask it.
     * radio_devices_docs/wl55_device/security/README.md */
    memset(hdr, 0, sizeof(hdr));
    memset(ibuf, 0, sizeof(ibuf));
    memcpy(hdr, aad, aad_len);
    memcpy(ibuf, in, len);

    if (gcm_configure(k, iv, hdr, aad_len) != 0)
        return -1;
    if (decrypt) {
        if (HAL_CRYP_Decrypt(&hcryp, ibuf, len, obuf, 1000) != HAL_OK)
            return -1;
    } else {
        if (HAL_CRYP_Encrypt(&hcryp, ibuf, len, obuf, 1000) != HAL_OK)
            return -1;
    }
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&hcryp, t, 1000) != HAL_OK)
        return -1;
    memcpy(out, obuf, len);
    memcpy(tag, t, 16);
    return 0;
}

static int gcm_run(const uint8_t *key16, const uint8_t *nonce12,
                   const uint8_t *aad, uint32_t aad_len,
                   const uint8_t *in, uint16_t len, uint8_t *out, uint8_t *tag,
                   int decrypt) {
    load_enter(LOAD_CRYPTO);
    int rc = gcm_run_inner(key16, nonce12, aad, aad_len, in, len, out, tag, decrypt);
    load_exit();
    return rc;
}

int crypto_gcm_seal(const uint8_t *key16, const uint8_t *nonce12,
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *pt, uint16_t len,
                    uint8_t *ct, uint8_t *tag) {
    return gcm_run(key16, nonce12, aad, aad_len, pt, len, ct, tag, 0);
}

int crypto_gcm_open(const uint8_t *key16, const uint8_t *nonce12,
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *ct, uint16_t len,
                    uint8_t *pt, const uint8_t *tag) {
    uint8_t computed[16];
    if (gcm_run(key16, nonce12, aad, aad_len, ct, len, pt, computed, 1) != 0)
        return -1;
    /* Constant time: where a forged tag first differs recovers it a byte at a time.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= (uint8_t)(computed[i] ^ tag[i]);
    if (diff != 0) {
        memset(pt, 0, len);
        return -2;
    }
    return 0;
}

/* Constant time over secret scalars: an early exit leaks where they differ.
 * radio_devices_docs/wl55_device/security/self-tests.md */
static int be_less(const uint8_t *a, const uint8_t *b) {
    uint8_t lt = 0, gt = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t ai = a[i], bi = b[i];
        uint8_t eq = (uint8_t)~(lt | gt);
        lt |= (uint8_t)(eq & (uint8_t)(0u - (uint8_t)(ai < bi)));
        gt |= (uint8_t)(eq & (uint8_t)(0u - (uint8_t)(ai > bi)));
    }
    return lt != 0;
}

static int is_zero(const uint8_t *a, uint32_t len) {
    uint8_t acc = 0;
    for (uint32_t i = 0; i < len; i++)
        acc |= a[i];
    return acc == 0;
}

static int scalar_mul(const uint8_t *scalar, const uint8_t *px, const uint8_t *py,
                      uint8_t *qx, uint8_t *qy) {
    PKA_ECCMulInTypeDef in = {0};
    PKA_ECCMulOutTypeDef out = {0};

    in.scalarMulSize = 32;
    in.modulusSize   = sizeof(p256_prime);
    in.coefSign      = 1;
    in.coefA         = p256_abs_a;
    in.modulus       = p256_prime;
    in.pointX        = px;
    in.pointY        = py;
    in.scalarMul     = scalar;
    if (pka_eccmul_timed(&hpka, &in, 5000) != HAL_OK)
        return -1;
    out.ptX = qx;
    out.ptY = qy;
    HAL_PKA_ECCMul_GetResult(&hpka, &out);
    return 0;
}

/* 256-bit big-endian helpers; everything expensive goes to the PKA. */
static uint8_t be_add(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    uint32_t carry = 0;
    for (int i = 31; i >= 0; i--) {
        uint32_t v = (uint32_t)a[i] + b[i] + carry;
        out[i] = (uint8_t)v;
        carry = v >> 8;
    }
    return (uint8_t)carry;
}

static uint8_t be_sub(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    int32_t borrow = 0;
    for (int i = 31; i >= 0; i--) {
        int32_t v = (int32_t)a[i] - b[i] - borrow;
        borrow = (v < 0);
        out[i] = (uint8_t)(v + (borrow ? 256 : 0));
    }
    return (uint8_t)borrow;
}

/* out = (a + b) mod p, where a and b are already reduced. */
static void mod_add(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    uint8_t t[32];
    uint8_t carry = be_add(a, b, t);
    uint8_t u[32];
    uint8_t borrow = be_sub(t, p256_prime, u);
    /* Take the reduced value if the sum overflowed or did not underflow. */
    int use_u = carry || !borrow;
    memcpy(out, use_u ? u : t, 32);
}

static void mod_sub(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    uint8_t t[32];
    if (be_sub(a, b, t))
        be_add(t, p256_prime, t);
    memcpy(out, t, 32);
}

static int mod_exp(const uint8_t *base, const uint8_t *exp, uint8_t *out) {
    PKA_ModExpInTypeDef in = {0};
    in.expSize = 32;
    in.OpSize  = 32;
    in.pExp    = exp;
    in.pOp1    = base;
    in.pMod    = p256_prime;
    if (pka_modexp_timed(&hpka, &in, 5000) != HAL_OK)
        return -1;
    HAL_PKA_ModExp_GetResult(&hpka, out);
    return 0;
}

int crypto_p256_decompress(uint8_t prefix, const uint8_t *x, uint8_t *sec1_out) {
    static const uint8_t three[32] = {[31] = 3};
    uint8_t t[32], x3[32], tx[32], y[32], neg[32];

    if (prefix != 0x02u && prefix != 0x03u)
        return -1;
    /* X is attacker supplied and must be a field element before the curve equation runs.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    if (!be_less(x, p256_prime))
        return -2;

    if (mod_exp(x, three, x3) != 0)          /* x^3 mod p */
        return -1;
    mod_add(x, x, tx);                        /* 2x */
    mod_add(tx, x, tx);                       /* 3x, since a = -3 */
    mod_sub(x3, tx, t);
    mod_add(t, p256_b, t);                    /* t = x^3 - 3x + b */

    if (mod_exp(t, p256_sqrt_exp, y) != 0)
        return -1;

    /* p = 3 mod 4 gives a root only when one exists, so square it back and check.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t check[32];
    if (mod_exp(y, (const uint8_t[32]){[31] = 2}, check) != 0)
        return -1;
    if (memcmp(check, t, 32) != 0)
        return -2;                            /* X is not on the curve */

    if ((y[31] & 1u) != (prefix & 1u)) {
        mod_sub(p256_prime, y, neg);
        memcpy(y, neg, 32);
    }
    if (is_zero(y, 32))
        return -2;                            /* the identity is not a key */

    sec1_out[0] = 0x04u;
    memcpy(sec1_out + 1, x, 32);
    memcpy(sec1_out + 33, y, 32);
    return 0;
}

int crypto_p256_public_from_private(const uint8_t *priv, uint8_t *pub_sec1) {
    pub_sec1[0] = 0x04u;
    return scalar_mul(priv, p256_gx, p256_gy, pub_sec1 + 1, pub_sec1 + 33);
}

/* SEIS latches while idle and HAL_RNG_Generate still returns HAL_OK.
 * radio_devices_docs/wl55_device/security/self-tests.md */
int crypto_rng_word(uint32_t *out) {
    for (int tries = 0; tries < 4; tries++) {
        uint32_t stale = 0;
        RNG->SR &= ~RNG_SR_SEIS;
        (void)HAL_RNG_GenerateRandomNumber(&hrng, &stale);
        if (HAL_RNG_GenerateRandomNumber(&hrng, out) != HAL_OK)
            continue;
        if (RNG->SR & (RNG_SR_SEIS | RNG_SR_SECS))
            continue;
        return 0;
    }
    *out = 0;
    return -1;
}

int crypto_rng_health(uint32_t *sr) {
    *sr = RNG->SR;
    return (*sr & (RNG_SR_SEIS | RNG_SR_SECS | RNG_SR_CEIS | RNG_SR_CECS)) ? -1 : 0;
}

int crypto_p256_keygen(uint8_t *priv, uint8_t *pub_sec1) {
    /* Rejection sampling: clamping into range would bias it, and the bias is the attack.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    for (int tries = 0; tries < 16; tries++) {
        for (int i = 0; i < 8; i++) {
            uint32_t w = 0;
            if (crypto_rng_word(&w) != 0) {
                /* The whole scalar: part-fresh, part-stack is a plausible key nothing can spot.
                 * radio_devices_docs/wl55_device/security/self-tests.md */
                memset(priv, 0, 32);
                return -1;
            }
            priv[4 * i]     = (uint8_t)(w >> 24);
            priv[4 * i + 1] = (uint8_t)(w >> 16);
            priv[4 * i + 2] = (uint8_t)(w >> 8);
            priv[4 * i + 3] = (uint8_t)w;
        }
        if (is_zero(priv, 32) || !be_less(priv, p256_order))
            continue;
        pub_sec1[0] = 0x04u;
        if (scalar_mul(priv, p256_gx, p256_gy, pub_sec1 + 1, pub_sec1 + 33) != 0)
            return -1;
        return 0;
    }
    return -1;
}

int crypto_p256_ecdh(const uint8_t *priv, const uint8_t *peer_sec1, uint8_t *shared_x) {
    PKA_PointCheckInTypeDef chk = {0};
    uint8_t qy[32];

    /* Validate before use, every time: an unvalidated point recovers the private key.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    if (peer_sec1[0] != 0x04u)
        return -1;
    chk.modulusSize = sizeof(p256_prime);
    chk.coefSign    = 1;
    chk.coefA       = p256_abs_a;
    chk.coefB       = p256_b;
    chk.modulus     = p256_prime;
    chk.pointX      = peer_sec1 + 1;
    chk.pointY      = peer_sec1 + 33;
    if (pka_pointcheck_timed(&hpka, &chk, 5000) != HAL_OK)
        return -1;
    if (HAL_PKA_PointCheck_IsOnCurve(&hpka) != 1u)
        return -2;

    if (scalar_mul(priv, peer_sec1 + 1, peer_sec1 + 33, shared_x, qy) != 0)
        return -1;
    /* The point at infinity would come back as all zeros and must not be used. */
    if (is_zero(shared_x, 32) && is_zero(qy, 32))
        return -2;
    return 0;
}

int crypto_wire_kat(crypto_wire_result_t *r) {
    PKA_ECCMulInTypeDef mul = {0};
    PKA_PointCheckInTypeDef chk = {0};
    PKA_ECCMulOutTypeDef out = {0};
    uint8_t qx[32], qy[32];
    uint8_t key[16], tag[16], cipher[sizeof(V_PLAIN)];

    memset(r, 0, sizeof(*r));
    uint32_t t0 = micros();

    /* SEC1 0x04 || X || Y, validated before it is touched.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    chk.modulusSize = sizeof(p256_prime);
    chk.coefSign    = 1;
    chk.coefA       = p256_abs_a;
    chk.coefB       = p256_b;
    chk.modulus     = p256_prime;
    chk.pointX      = &V_DEV_PUB[1];
    chk.pointY      = &V_DEV_PUB[33];
    if (V_DEV_PUB[0] != 0x04u)
        return -1;
    if (pka_pointcheck_timed(&hpka, &chk, 5000) != HAL_OK)
        return -1;
    r->point_valid = (HAL_PKA_PointCheck_IsOnCurve(&hpka) == 1u);

    mul.scalarMulSize = sizeof(V_HUB_PRIV);
    mul.modulusSize   = sizeof(p256_prime);
    mul.coefSign      = 1;
    mul.coefA         = p256_abs_a;
    mul.modulus       = p256_prime;
    mul.pointX        = &V_DEV_PUB[1];
    mul.pointY        = &V_DEV_PUB[33];
    mul.scalarMul     = V_HUB_PRIV;
    if (pka_eccmul_timed(&hpka, &mul, 5000) != HAL_OK)
        return -1;
    out.ptX = qx;
    out.ptY = qy;
    HAL_PKA_ECCMul_GetResult(&hpka, &out);
    /* SEC1 ECDH takes the X coordinate alone - not the point, not a hash. */
    r->ecdh_ok = (memcmp(qx, V_ECDH_X, sizeof(V_ECDH_X)) == 0);

    hkdf_sha256(V_SALT, sizeof(V_SALT), V_ECDH_X, sizeof(V_ECDH_X),
                V_INFO_SESSION, sizeof(V_INFO_SESSION), key, sizeof(key));
    r->session_ok = (memcmp(key, V_KEY_SESSION0, sizeof(key)) == 0);

    uint8_t hop[16];
    hkdf_sha256(V_SALT, sizeof(V_SALT), V_ECDH_X, sizeof(V_ECDH_X),
                V_INFO_HOP, sizeof(V_INFO_HOP), hop, sizeof(hop));
    r->hop_ok = (memcmp(hop, V_KEY_HOP0, sizeof(hop)) == 0);

    uint8_t gen1[16];
    hkdf_sha256(NULL, 0, V_KEY_SESSION0, sizeof(V_KEY_SESSION0),
                V_INFO_ROTATE, sizeof(V_INFO_ROTATE), gen1, sizeof(gen1));
    r->ratchet_ok = (memcmp(gen1, V_KEY_SESSION1, sizeof(gen1)) == 0);

    if (crypto_gcm_seal(V_KEY_SESSION0, V_NONCE, V_AAD, sizeof(V_AAD),
                        V_PLAIN, sizeof(V_PLAIN), cipher, tag) != 0)
        return -1;
    r->cipher_ok = (memcmp(cipher, V_CIPHER, sizeof(V_CIPHER)) == 0);
    r->tag_ok    = (memcmp(tag, V_TAG, sizeof(V_TAG)) == 0);

    /* Proves the open path against the same bytes, and that a flipped tag is rejected.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t plain[sizeof(V_PLAIN)];
    r->open_ok = (crypto_gcm_open(V_KEY_SESSION0, V_NONCE, V_AAD, sizeof(V_AAD),
                                  V_CIPHER, sizeof(V_CIPHER), plain, V_TAG) == 0) &&
                 (memcmp(plain, V_PLAIN, sizeof(V_PLAIN)) == 0);
    uint8_t bad_tag[16];
    memcpy(bad_tag, V_TAG, sizeof(bad_tag));
    bad_tag[0] ^= 0x01u;
    r->forge_rejected = (crypto_gcm_open(V_KEY_SESSION0, V_NONCE, V_AAD, sizeof(V_AAD),
                                         V_CIPHER, sizeof(V_CIPHER), plain, bad_tag) == -2);

    /* 23 bytes, not a whole number of words. The v1 payload was 24 and always passed.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t odd_ct[sizeof(V_ODD_PLAIN)], odd_tag[16], odd_pt[sizeof(V_ODD_PLAIN)];
    if (crypto_gcm_seal(V_KEY_SESSION0, V_ODD_NONCE, V_AAD, sizeof(V_AAD),
                        V_ODD_PLAIN, sizeof(V_ODD_PLAIN), odd_ct, odd_tag) != 0)
        return -1;
    r->odd_seal_ok = (memcmp(odd_ct, V_ODD_CIPHER, sizeof(V_ODD_CIPHER)) == 0) &&
                     (memcmp(odd_tag, V_ODD_TAG, sizeof(odd_tag)) == 0);
    r->odd_open_ok = (crypto_gcm_open(V_KEY_SESSION0, V_ODD_NONCE, V_AAD, sizeof(V_AAD),
                                      V_ODD_CIPHER, sizeof(V_ODD_CIPHER),
                                      odd_pt, V_ODD_TAG) == 0) &&
                     (memcmp(odd_pt, V_ODD_PLAIN, sizeof(V_ODD_PLAIN)) == 0);

    /* Measured: the hub needs the number to decide whether compressed points fit here.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t decomp[P256_PUB_LEN];
    uint32_t td = micros();
    int drc = crypto_p256_decompress(vec_p256_prefix, vec_p256_qx, decomp);
    r->decompress_us = micros() - td;
    r->decompress_ok = (drc == 0) &&
                       (memcmp(decomp + 1, vec_p256_qx, 32) == 0) &&
                       (memcmp(decomp + 33, vec_p256_qy, 32) == 0);
    /* Flipping the parity bit must give the other root, not the same point. */
    uint8_t other[P256_PUB_LEN];
    r->decompress_parity_ok =
        (crypto_p256_decompress((uint8_t)(vec_p256_prefix ^ 1u), vec_p256_qx, other) == 0) &&
        (memcmp(other + 33, vec_p256_qy, 32) != 0);
    /* A genuine non-residue: half of all field elements are valid x, so a flip passes.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    int bad_rc = crypto_p256_decompress(0x02u, vec_p256_x_no_root, other);
    /* And an X that is not even a field element must go too. */
    int oob_rc = crypto_p256_decompress(0x02u, p256_prime, other);
    r->decompress_reject_ok = (bad_rc == -2) && (oob_rc == -2);

    /* The hub's bytes, not my own arithmetic: agreeing with myself proves nothing.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t shared_pub[P256_PUB_LEN];
    r->shared_decomp_ok =
        (crypto_p256_decompress(V_DEV_PUB_C[0], V_DEV_PUB_C + 1, shared_pub) == 0) &&
        (memcmp(shared_pub + 1, V_DEV_PUB_X, 32) == 0) &&
        (memcmp(shared_pub + 33, V_DEV_PUB_Y, 32) == 0);

    /* ECDH on the decompressed point: a wrong Y cannot survive that, a Y compare can.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t x_from_c[32];
    r->decomp_ecdh_ok =
        (crypto_p256_ecdh(V_HUB_PRIV, shared_pub, x_from_c) == 0) &&
        (memcmp(x_from_c, V_ECDH_X, sizeof(V_ECDH_X)) == 0);

    /* Their non-residue, so both sides refuse the same bytes. */
    r->shared_reject_ok =
        (crypto_p256_decompress(V_REJECT_C[0], V_REJECT_C + 1, shared_pub) == -2);

    r->total_us = micros() - t0;
    r->digest = WIRE_VECTORS_DIGEST;
    return 0;
}

#include "exchange.h"
#include "pair_v2.h"
#include "hop_vectors.h"

/* pair_v2 on this silicon: the schedule, and the two sealed frames through CRYP.
 * radio_devices_docs/wl55_device/security/self-tests.md */
int crypto_pair_kat(crypto_pair_result_t *r) {
    uint8_t salt[EXCHANGE_SALT_LEN];
    uint8_t transcript[EXCHANGE_TRANSCRIPT_LEN];
    uint8_t buf[32];
    uint8_t tag[16];
    exchange_keys_t k;
    uint32_t t0 = micros();

    memset(r, 0, sizeof(*r));
    r->digest = PAIR_VECTORS_DIGEST;

    exchange_salt(0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME, PV_DEV_NONCE, salt);
    r->salt_ok = memcmp(salt, PV_SALT, sizeof(PV_SALT)) == 0;
    exchange_transcript(0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME, PV_DEV_NONCE,
                        V_HUB_PUB_C, PV_HUB_EPH_PUB, V_DEV_PUB_C, transcript);
    r->transcript_ok = memcmp(transcript, PV_TRANSCRIPT, sizeof(PV_TRANSCRIPT)) == 0;

    exchange_derive(PV_Z, PV_Z + EXCHANGE_Z_TERM_LEN, salt, transcript, &k);
    r->session_ok     = memcmp(k.session, PV_KEY_SESSION, 16) == 0;
    r->confirm_hub_ok = memcmp(k.confirm_hub, PV_CONFIRM_HUB, 16) == 0;
    r->confirm_dev_ok = memcmp(k.confirm_dev, PV_CONFIRM_DEV, 16) == 0;

    /* 19 bytes, three past a word boundary; the tag is what catches the unmasked word.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    memset(buf, 0, sizeof(buf));
    r->accept_open_ok =
        crypto_gcm_open(PV_KEY_SESSION, PV_ACCEPT_NONCE,
                        PV_ACCEPT_AAD, sizeof(PV_ACCEPT_AAD),
                        PV_FRAME_ACCEPT + sizeof(PV_ACCEPT_AAD),
                        (uint16_t)sizeof(PV_ACCEPT_PLAIN), buf,
                        PV_FRAME_ACCEPT + sizeof(PV_ACCEPT_AAD)
                                        + sizeof(PV_ACCEPT_PLAIN)) == 0 &&
        memcmp(buf, PV_ACCEPT_PLAIN, sizeof(PV_ACCEPT_PLAIN)) == 0;

    /* The hop key is the last 16 bytes: an offset error costs the whole channel plan.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    r->hop_key_ok = memcmp(buf + 3, PV_NET_HOP_KEY, sizeof(PV_NET_HOP_KEY)) == 0;

    memcpy(tag, PV_FRAME_ACCEPT + sizeof(PV_ACCEPT_AAD) + sizeof(PV_ACCEPT_PLAIN), 16);
    tag[0] ^= 0x01u;
    r->accept_forge_rejected =
        crypto_gcm_open(PV_KEY_SESSION, PV_ACCEPT_NONCE,
                        PV_ACCEPT_AAD, sizeof(PV_ACCEPT_AAD),
                        PV_FRAME_ACCEPT + sizeof(PV_ACCEPT_AAD),
                        (uint16_t)sizeof(PV_ACCEPT_PLAIN), buf, tag) != 0;

    /* Sealing against published bytes proves the assembly; a round trip proves neither.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    uint8_t ct[sizeof(PV_UPLINK_PLAIN)];
    r->uplink_seal_ok =
        crypto_gcm_seal(PV_KEY_SESSION, PV_UPLINK_NONCE,
                        PV_UPLINK_AAD, sizeof(PV_UPLINK_AAD),
                        PV_UPLINK_PLAIN, (uint16_t)sizeof(PV_UPLINK_PLAIN),
                        ct, tag) == 0 &&
        memcmp(ct, PV_FRAME_UPLINK + sizeof(PV_UPLINK_AAD), sizeof(ct)) == 0 &&
        memcmp(tag, PV_FRAME_UPLINK + sizeof(PV_UPLINK_AAD) + sizeof(ct), 16) == 0;

    r->eph_static_rejected = exchange_eph_is_static(V_HUB_PUB_C, V_HUB_PUB_C) == 1 &&
                             exchange_eph_is_static(PV_HUB_EPH_PUB, V_HUB_PUB_C) == 0;

    /* Leave CRYP hostile, then open again: inherited settings do not fail.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    hcryp.Init.DataType        = CRYP_DATATYPE_32B;
    hcryp.Init.DataWidthUnit   = CRYP_DATAWIDTHUNIT_WORD;
    hcryp.Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_WORD;
    hcryp.Init.Algorithm       = CRYP_AES_ECB;
    (void)HAL_CRYP_Init(&hcryp);
    /* ECB first, and the order is the test: a GCM open re-initialises the peripheral.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    memset(buf, 0, sizeof(buf));
    r->after_misconfig_ok =
        crypto_aes_ecb_block(vec_hop_key, vec_hop_prf_in, buf) == 0 &&
        memcmp(buf, vec_hop_prf_out, sizeof(vec_hop_prf_out)) == 0;

    /* FIPS-197 C.1: this silicon is the anchor, not the host assert.
     * radio_devices_docs/wl55_device/security/self-tests.md */
    memset(buf, 0, sizeof(buf));
    r->fips_ok = crypto_aes_ecb_block(vec_aes_fips_key, vec_aes_fips_in, buf) == 0 &&
                 memcmp(buf, vec_aes_fips_out, sizeof(vec_aes_fips_out)) == 0;

    hcryp.Init.DataType        = CRYP_DATATYPE_32B;
    hcryp.Init.DataWidthUnit   = CRYP_DATAWIDTHUNIT_WORD;
    hcryp.Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_WORD;
    hcryp.Init.Algorithm       = CRYP_AES_ECB;
    (void)HAL_CRYP_Init(&hcryp);
    memset(buf, 0, sizeof(buf));
    r->after_misconfig_ok = r->after_misconfig_ok &&
        crypto_gcm_open(PV_KEY_SESSION, PV_ACCEPT_NONCE,
                        PV_ACCEPT_AAD, sizeof(PV_ACCEPT_AAD),
                        PV_FRAME_ACCEPT + sizeof(PV_ACCEPT_AAD),
                        (uint16_t)sizeof(PV_ACCEPT_PLAIN), buf,
                        PV_FRAME_ACCEPT + sizeof(PV_ACCEPT_AAD)
                                        + sizeof(PV_ACCEPT_PLAIN)) == 0 &&
        memcmp(buf, PV_ACCEPT_PLAIN, sizeof(PV_ACCEPT_PLAIN)) == 0;

    r->total_us = micros() - t0;
    return 0;
}
