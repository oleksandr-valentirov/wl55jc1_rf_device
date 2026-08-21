#pragma once

#include <stdint.h>

/* Records the device emits on its own, so the host is not asking.
 * radio_devices_docs/wl55_device/testing/telemetry.md */

#define TLM_LINE_MAX  96
#define TLM_FIELDS    4
#define TLM_RING      32

typedef enum {
    TLM_BOOT = 0,
    TLM_BEAT,
    TLM_SYNC_LOST,
    TLM_SYNC_OK,
    TLM_SYNC_JUMP,
    TLM_REC_ENTER,
    TLM_REC_PARK,
    TLM_REC_HIT,
    TLM_REC_DENY,
    TLM_REC_EXIT,
    TLM_TX_UP,
    TLM_TX_DENY,
    TLM_RX_MISS,
    TLM_RX_CMD,
    TLM_RX_BEACON,
    TLM_TX_ARM,
    TLM_KIND_COUNT
} tlm_kind_t;

/* Why a frame was turned away, carried in the `why` field. */
#define TLM_WHY_CHANNEL   1u   /* the counter does not map to this channel */
#define TLM_WHY_BEACON    2u   /* beacon_apply refused; its rc is the next field */
#define TLM_WHY_SEAL      3u   /* the superframe was already sealed under */
#define TLM_WHY_RADIO     4u
#define TLM_WHY_BAND      5u   /* the band filter held this cycle back */
#define TLM_WHY_LATE      6u   /* the opportunity's slot had already passed */
#define TLM_WHY_OFFBEAT   7u   /* the aligned counter is not a reporting one */
#define TLM_WHY_RESERVE   8u   /* the counter is outside the reserved window */

/* rx.cmd `rpt`: 0 applied, 1 a repeat of the one held, 2 a keepalive. */

/** @brief Rings a record: a timestamp, a sequence number and four words. */
void tlm_emit(tlm_kind_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/** @brief Formats one pending line, or returns 0 when the ring is empty. */
int tlm_next(char *buf, int max);

/** @brief Silences or resumes the stream; for a measurement needing a quiet console. */
void     tlm_enable(uint8_t on);

/** @brief Whether records are currently being emitted. */
uint8_t  tlm_enabled(void);

/** @brief Records lost to a full ring; the cross-check, not the evidence. */
uint32_t tlm_dropped(void);

/** @brief The next sequence number; a gap in the stream is the loss signal. */
uint32_t tlm_seq(void);

/** @brief Test seam: the ring is static and a host test needs a known state. */
void tlm_reset(void);
