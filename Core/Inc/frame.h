#pragma once

#include <stdint.h>

#include "crypto.h"
#include "radio_protocol.h"

#define FRAME_HEADER_LEN    8u
#define FRAME_TAG_LEN       16u
#define FRAME_MAX_PAYLOAD   CRYPTO_GCM_MAX_LEN
#define FRAME_MAX_LEN       (FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD + FRAME_TAG_LEN)

#define FRAME_TYPE_DATA     0x01u
/* From the shared header, never a second opinion held here. A version and a
 * direction each side defines for itself is the 45-vs-49 shape: two asserts
 * pass, both sides agree with themselves, and the frame is refused on air. */
#define FRAME_VERSION       RADIO_PROTO_VERSION
#define FRAME_DIR_UPLINK    RADIO_DIR_UPLINK
#define FRAME_DIR_DOWNLINK  RADIO_DIR_DOWNLINK

/* The header as transmitted. Little-endian, because these are wire fields -
 * the nonce built from the same values is big-endian, which is the rule and
 * not an inconsistency. */
typedef struct {
    uint8_t  type;
    uint8_t  version;
    uint16_t net_id;
    uint32_t dev_id;
} __attribute__((packed)) frame_header_t;

typedef struct {
    uint8_t  key[16];
    uint32_t dev_id;
    uint16_t net_id;
    uint32_t superframe;      /* the counter this end believes it is on */
    uint32_t tx_floor;        /* refuse to seal below this; it may already be used */
    /* And refuse at or above this. The floor alone is only half the guarantee:
     * flash stores a *ceiling* that has been reserved, so counters past it have
     * not been persisted and a reboot would hand them out a second time. A floor
     * with no ceiling makes the store's whole purpose conditional on something
     * that is not checked anywhere. */
    uint32_t tx_mark;
    uint32_t last_accepted;   /* highest counter already opened, for replay */
    uint8_t  have_accepted;
} frame_ctx_t;

/** @brief Binds the session key and the identity a frame is built under. */
void frame_init(frame_ctx_t *ctx, const uint8_t *key, uint32_t dev_id, uint16_t net_id);

/** @brief Loads the durable replay floors; without them a reset reuses a nonce.
 *  radio_devices_docs/wl55_device/arch/store.md */
void frame_set_floors(frame_ctx_t *ctx, uint32_t tx_floor, uint32_t rx_floor);

/** @brief Extends the reserved transmit window once flash holds a new ceiling. */
void frame_set_tx_mark(frame_ctx_t *ctx, uint32_t mark);

/** @brief Whether this counter may be sealed under; askable before building. */
int  frame_tx_allowed(const frame_ctx_t *ctx, uint32_t superframe);

/** @brief Seals a payload.
 *  @return the frame length, or 0 if the payload does not fit */
uint16_t frame_seal(frame_ctx_t *ctx, uint8_t direction, uint32_t slot,
                    const uint8_t *payload, uint8_t len, uint8_t *out);

/** @brief Opens a frame; 0 ok, -1 malformed, -2 tag failed, -3 replay. */
int frame_open(frame_ctx_t *ctx, const uint8_t *in, uint16_t in_len,
               uint32_t superframe, uint8_t direction, uint32_t slot,
               uint8_t *payload, uint8_t *out_len);
