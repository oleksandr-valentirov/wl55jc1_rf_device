#pragma once

#include <stdint.h>

#include "crypto.h"
#include "sha256.h"
#include "radio_protocol.h"

/* Cleartext by necessity: these are the frames exchanged before a key exists.
 * Authentication does not come from the frame, it comes from the operator
 * having supplied the device's key fingerprint out of band. */
#define PAIR_TYPE_REQUEST   0x03u
#define PAIR_TYPE_RESPONSE  0x04u
/* The shared header owns it: see frame.h. */
#define PAIR_VERSION        RADIO_PROTO_VERSION
#define PAIR_NONCE_LEN      ((uint32_t)sizeof(((radio_pair_req_t *)0)->dev_nonce))
/* 49 bytes, and the layout comes from radio_pair_req_t in the shared header
 * rather than from a local idea of it. This was 45 - a private layout with the
 * pubkey at offset 12, where the hub reads a superframe - so the two ends each
 * asserted their own length and agreed with themselves. The frame would have
 * been refused on length, which is a clean failure and an undiagnosable one:
 * pairing simply never works. */
#define PAIR_FRAME_LEN      ((uint16_t)sizeof(radio_pair_req_t))

typedef struct {
    uint8_t priv[P256_PRIV_LEN];
    uint8_t pub[P256_PUB_LEN];
    uint8_t have_key;
    uint8_t session[16];
    uint8_t hop[16];
    uint8_t paired;
    uint32_t dev_id;
    uint32_t hub_id;
    uint16_t net_id;
    uint32_t superframe;    /* carried so a joiner is aligned before it has a key */
    uint8_t  dev_nonce[8];  /* the device's only freshness; see radio_pair_req_t */
} pairing_ctx_t;

/** @brief Generates the ephemeral keypair for one exchange. */
int pairing_keygen(pairing_ctx_t *ctx);
/** @brief Draws a fresh dev_nonce; -1 and a zero nonce when the TRNG refuses. */
int pairing_new_nonce(pairing_ctx_t *ctx);
/** @brief Names the hub this exchange is with. */
int pairing_set_hub(pairing_ctx_t *ctx, uint32_t hub_id);
/** @brief Checks the salt is assembled as the contract publishes it. */
int pairing_salt_check(const uint8_t *shared_x, uint32_t hub_id, uint32_t dev_id,
                       uint8_t *session_out, uint8_t *hop_out);
/** @brief The out-of-band fingerprint: all SHA256_LEN bytes, never a prefix. */
uint8_t pairing_fingerprint(const pairing_ctx_t *ctx, uint8_t out[SHA256_LEN],
                            uint32_t out_len);
/** @brief This device's public key in compressed form. */
uint8_t pairing_pubkey_c(const pairing_ctx_t *ctx,
                         uint8_t out[P256_PUB_COMPRESSED_LEN],
                         uint32_t out_len);

/** @brief The one-frame bench exchange, NOT the protocol's pairing.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int pairing_run_device(pairing_ctx_t *ctx, uint32_t timeout_ms);
/** @brief The bench exchange's hub side, for testing without the real hub. */
int pairing_run_hub(pairing_ctx_t *ctx, uint32_t timeout_ms);
