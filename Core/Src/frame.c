/* header (8, AAD) || ciphertext (n) || tag (16). The nonce is never transmitted.
 * radio_devices_docs/wl55_device/security/README.md */
#include <string.h>

#include "frame.h"

void frame_init(frame_ctx_t *ctx, const uint8_t *key, uint32_t dev_id, uint16_t net_id) {
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->key, key, sizeof(ctx->key));
    ctx->dev_id = dev_id;
    ctx->net_id = net_id;
}

/* Big-endian end to end: a crypto input, not a wire field.
 * radio_devices_docs/wl55_device/security/README.md */
static void build_nonce(uint32_t superframe, uint32_t dev_id, uint8_t direction,
                        uint32_t slot, uint8_t *nonce) {
    nonce[0]  = (uint8_t)(superframe >> 24);
    nonce[1]  = (uint8_t)(superframe >> 16);
    nonce[2]  = (uint8_t)(superframe >> 8);
    nonce[3]  = (uint8_t)superframe;
    nonce[4]  = (uint8_t)(dev_id >> 24);
    nonce[5]  = (uint8_t)(dev_id >> 16);
    nonce[6]  = (uint8_t)(dev_id >> 8);
    nonce[7]  = (uint8_t)dev_id;
    nonce[8]  = direction;
    nonce[9]  = (uint8_t)(slot >> 16);
    nonce[10] = (uint8_t)(slot >> 8);
    nonce[11] = (uint8_t)slot;
}

/* The struct documents; build_header writes. FRAME_HEADER_LEN is what GCM covers.
 * radio_devices_docs/wl55_device/security/README.md */
_Static_assert(sizeof(frame_header_t) == FRAME_HEADER_LEN,
               "header struct and the authenticated length must agree");

static void build_header(const frame_ctx_t *ctx, uint8_t *hdr) {
    hdr[0] = FRAME_TYPE_DATA;
    hdr[1] = FRAME_VERSION;
    hdr[2] = (uint8_t)ctx->net_id;             /* little-endian: wire field */
    hdr[3] = (uint8_t)(ctx->net_id >> 8);
    hdr[4] = (uint8_t)ctx->dev_id;
    hdr[5] = (uint8_t)(ctx->dev_id >> 8);
    hdr[6] = (uint8_t)(ctx->dev_id >> 16);
    hdr[7] = (uint8_t)(ctx->dev_id >> 24);
}

void frame_set_tx_mark(frame_ctx_t *ctx, uint32_t mark) {
    ctx->tx_mark = mark;
}

int frame_tx_allowed(const frame_ctx_t *ctx, uint32_t superframe) {
    /* Below the floor the counter may be spent; at or above the mark it is unpersisted.
     * radio_devices_docs/wl55_device/arch/store.md */
    if (ctx->tx_floor != 0u && (int32_t)(superframe - ctx->tx_floor) < 0)
        return 0;
    if (ctx->tx_mark != 0u && (int32_t)(superframe - ctx->tx_mark) >= 0)
        return 0;
    return 1;
}

void frame_set_floors(frame_ctx_t *ctx, uint32_t tx_floor, uint32_t rx_floor) {
    ctx->tx_floor = tx_floor;
    if (rx_floor != 0u) {
        ctx->last_accepted = rx_floor;
        ctx->have_accepted = 1;
    }
}

uint16_t frame_seal(frame_ctx_t *ctx, uint8_t direction, uint32_t slot,
                    const uint8_t *payload, uint8_t len, uint8_t *out) {
    uint8_t nonce[12];

    if (len > FRAME_MAX_PAYLOAD)
        return 0;
    /* A repeated GCM nonce loses the authentication subkey, not one plaintext.
     * radio_devices_docs/wl55_device/arch/store.md */
    if (!frame_tx_allowed(ctx, ctx->superframe))
        return 0;
    build_header(ctx, out);
    build_nonce(ctx->superframe, ctx->dev_id, direction, slot, nonce);
    if (crypto_gcm_seal(ctx->key, nonce, out, FRAME_HEADER_LEN,
                        payload, len, out + FRAME_HEADER_LEN,
                        out + FRAME_HEADER_LEN + len) != 0)
        return 0;
    return (uint16_t)(FRAME_HEADER_LEN + len + FRAME_TAG_LEN);
}

int frame_open(frame_ctx_t *ctx, const uint8_t *in, uint16_t in_len,
               uint32_t superframe, uint8_t direction, uint32_t slot,
               uint8_t *payload, uint8_t *out_len) {
    uint8_t nonce[12];

    if (in_len < FRAME_HEADER_LEN + FRAME_TAG_LEN)
        return -1;
    uint16_t len = (uint16_t)(in_len - FRAME_HEADER_LEN - FRAME_TAG_LEN);
    if (len > FRAME_MAX_PAYLOAD)
        return -1;
    if (in[0] != FRAME_TYPE_DATA || in[1] != FRAME_VERSION)
        return -1;

    build_nonce(superframe, ctx->dev_id, direction, slot, nonce);
    int rc = crypto_gcm_open(ctx->key, nonce, in, FRAME_HEADER_LEN,
                             in + FRAME_HEADER_LEN, len, payload,
                             in + FRAME_HEADER_LEN + len);
    if (rc != 0)
        return rc;

    /* Only after the tag: first would move this end's window with an unforgeable frame.
     * radio_devices_docs/wl55_device/security/README.md */
    if (ctx->have_accepted && (int32_t)(superframe - ctx->last_accepted) <= 0) {
        memset(payload, 0, len);
        return -3;
    }
    ctx->last_accepted = superframe;
    ctx->have_accepted = 1;
    *out_len = (uint8_t)len;
    return 0;
}
