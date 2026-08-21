#pragma once

#include <stdint.h>

#include "radio_protocol.h"
#include "sha256.h"

/* The pairing key schedule, written from the hub session's pair_v2 spec text
 * and not from its source. Two implementations agreeing is the only evidence
 * that the specification is complete.
 *
 * Deliberately free of crypto.h and of the HAL: everything here is SHA-256 and
 * byte layout, which is exactly where two independent implementations diverge
 * in silence. The two scalar multiplications that produce Z live in pairing.c,
 * so this file compiles and is tested on the host.
 *
 * v2 exists because v1 had no device-side freshness: Z and the transcript were
 * fixed by the two identities plus hub_eph, so a recorded PAIR_RSP re-derived
 * the same session key forever. The request's superframe and an 8-byte device
 * nonce now enter both the salt and the transcript. */

#define EXCHANGE_Z_TERM_LEN      32u
#define EXCHANGE_Z_LEN           (2u * EXCHANGE_Z_TERM_LEN)
#define EXCHANGE_NONCE_LEN       8u
#define EXCHANGE_CONFIRM_LEN     16u
#define EXCHANGE_CONFIRM_KEY_LEN 32u
#define EXCHANGE_KEY_LEN         16u

/* Taken from the wire struct rather than written as 33, so a compressed point
 * that ever changes width moves the transcript with it. */
#define EXCHANGE_POINT_LEN \
    ((uint32_t)sizeof(((radio_pair_req_t *)0)->pubkey))

/* hub_id | dev_id | superframe | dev_nonce, big-endian throughout. */
#define EXCHANGE_SALT_LEN        (4u + 4u + 4u + EXCHANGE_NONCE_LEN)

/* The salt, then hub_static | hub_eph | dev_static - hub keys before the
 * device's in every concatenation. Whichever key is not bound is not bound. */
#define EXCHANGE_TRANSCRIPT_LEN  (EXCHANGE_SALT_LEN + 3u * EXCHANGE_POINT_LEN)

/* No hop key. It is a network key delivered sealed in PAIR_ACCEPT, not a
 * pairwise one - deriving it here would be a key with no consumer, which is
 * how a pairwise hop key survived a byte-for-byte vector match. */
typedef struct {
    uint8_t session[EXCHANGE_KEY_LEN];
    uint8_t confirm_key_hub[EXCHANGE_CONFIRM_KEY_LEN];
    uint8_t confirm_key_dev[EXCHANGE_CONFIRM_KEY_LEN];
    uint8_t confirm_hub[EXCHANGE_CONFIRM_LEN];
    uint8_t confirm_dev[EXCHANGE_CONFIRM_LEN];
} exchange_keys_t;

/** @brief Builds the HKDF salt, separately from the transcript on purpose.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
void exchange_salt(uint32_t hub_id, uint32_t dev_id, uint32_t req_superframe,
                   const uint8_t *dev_nonce, uint8_t *out);

/** @brief Hashes the transcript both sides must agree on. */
void exchange_transcript(uint32_t hub_id, uint32_t dev_id, uint32_t req_superframe,
                         const uint8_t *dev_nonce,
                         const uint8_t *hub_static_c,
                         const uint8_t *hub_eph_c,
                         const uint8_t *dev_static_c,
                         uint8_t *out);

/** @brief Derives the session and hop keys; Z arrives as two terms, not one. */
void exchange_derive(const uint8_t *z1, const uint8_t *z2,
                     const uint8_t *salt, const uint8_t *transcript,
                     exchange_keys_t *out);

/** @brief Constant-time compare; returns 1 on a match, and acts on nothing. */
int exchange_confirm_equal(const uint8_t *a, const uint8_t *b);

/** @brief The hub reusing its static key as its ephemeral; NOT a freshness check.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int exchange_eph_is_static(const uint8_t *hub_eph_c, const uint8_t *hub_static_c);
