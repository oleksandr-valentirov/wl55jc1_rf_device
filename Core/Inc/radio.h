#pragma once

#include <stdint.h>

#include "radio_phy.h"

typedef struct {
    uint32_t timeout_us;   /* set by the caller before radio_receive */
    uint8_t  len;
    uint8_t  crc_error;
    int16_t  rssi_dbm;
    uint32_t done_us;      /* when RxDone was observed, for predicting the next */
    uint32_t capture_us;   /* the DIO1 edge, whichever event radio_set_capture picked */
    uint8_t  capture_valid;/* 0 when no interrupt was taken for this frame */
    uint8_t  capture_sync; /* 1 when capture_us is a sync-word edge, not a preamble one */
    uint32_t start_us;     /* the frame's first bit, from sync when there is one */
} radio_rx_info_t;

/* Bytes on air before the length field, so a sync-word timestamp converts to a
 * frame start. radio_devices_docs/wl55_device/radio/timebase.md */
#define RADIO_PRE_SYNC_US  ((RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES) * RADIO_US_PER_BYTE)

/** @brief Reads the chip's status byte. */
int radio_get_status(uint8_t *status);

/** @brief Reads one radio register over SUBGHZSPI. */
int radio_read_reg(uint16_t addr, uint8_t *value);

/** @brief Writes one radio register over SUBGHZSPI. */
int radio_write_reg(uint16_t addr, uint8_t value);

/** @brief Puts the radio in standby, leaving the configuration in place. */
int radio_standby(void);

/** @brief Reads and clears the chip's latched error word. */
int radio_get_error(uint16_t *err);

/** @brief A random word from the radio's own entropy source. */
int radio_rng_word(uint32_t *out);

/** @brief Configures the PHY and tunes to a grid slot. */
int      radio_configure(uint8_t slot);

/** @brief Configures bench mode: off the protocol's air, and a different sync word. */
int      radio_configure_bench(void);

/** @brief Whether bench mode is the current configuration. */
int      radio_bench_mode(void);

/** @brief Selects the CRC; a mismatch is dropped in hardware and counts nowhere.
 *  radio_devices_docs/radio/phy.md */
int         radio_set_crc(const char *mode);   /* "off" | "2" | "2inv" */

/** @brief Names the CRC currently configured. */
const char *radio_crc_name(void);

/** @brief Reads back what was staged for transmission. */
int  radio_read_tx_buffer(uint8_t *out, uint8_t len);

/** @brief Air time for a payload at the configured preamble and rate. */
uint32_t radio_air_time_us(uint8_t payload_len);

/** @brief Picks which edge DIO1 carries; only a sync word is a frame start.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
int         radio_set_capture(const char *src);   /* "sync" | "preamble" */

/** @brief Names the edge DIO1 is currently carrying. */
const char *radio_capture_name(void);

/** @brief Sets transmit power, -17 to +14 dBm on the low-power PA. */
int  radio_set_power(int dbm);

/** @brief The transmit power currently configured. */
int  radio_power_dbm(void);

/** @brief Sets the preamble; refused if the frame would outgrow the slot.
 *  radio_devices_docs/radio/phy.md */
int  radio_set_preamble(uint8_t bytes);

/** @brief The preamble length currently configured. */
uint8_t radio_preamble_bytes(void);

/** @brief The bench carrier, kept off the protocol's grid. */
uint32_t radio_bench_hz(void);

/** @brief Sets the reference offset; one offset on every tuning path. */
int      radio_set_freq_offset(int32_t hz);

/** @brief The reference offset currently applied. */
int32_t  radio_freq_offset(void);

/** @brief The TCXO startup wait, which is the whole of RX entry latency.
 *  radio_devices_docs/wl55_device/radio/README.md */
uint32_t radio_tcxo_us(void);

/** @brief Sets the TCXO wait; this reconfigures the radio. */
int      radio_set_tcxo_us(uint32_t us);

/** @brief Tunes to a grid slot without touching the rest of the PHY. */
int      radio_set_channel(uint8_t slot);

/** @brief The carrier for a grid slot. */
uint32_t radio_slot_hz(uint8_t slot);

/** @brief The grid slot reserved for joining, excluded from the hop set. */
uint8_t  radio_join_slot(void);

/** @brief Transmits a payload and reports the air time it took. */
int radio_send(const uint8_t *payload, uint8_t len, uint32_t *air_us);

/** @brief Receives one frame, blocking until info->timeout_us expires. */
int radio_receive(uint8_t *payload, uint8_t max_len, radio_rx_info_t *info);

/** @brief Enters RX at an absolute instant rather than now, for placed windows. */
int radio_receive_at(uint8_t *payload, uint8_t max_len, radio_rx_info_t *info,
                     uint32_t start_us);

/** @brief Times the chip's own timeout; the excess over it is the startup ramp.
 *  radio_devices_docs/wl55_device/radio/README.md */
int radio_rx_ramp_probe(uint32_t timeout_us, uint32_t *span_us);
