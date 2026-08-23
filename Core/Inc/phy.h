#pragma once

#include <stdint.h>

#include "radio_phy.h"

/**
 * @file phy.h
 * @brief The radio logic's whole view of a chip: nine operations and one event.
 *
 * radio_devices_docs/radio/phy-seam.md
 */

/* Every frame the protocol defines fits; the ceiling is the hub FIFO's, not this file's. */
#define PHY_MAX_PAYLOAD  RADIO_MAX_PAYLOAD_B

/** @brief What one poll found. A failed CRC is an event, never a silence. */
typedef enum {
    PHY_EV_NONE = 0,   /**< nothing since the last poll */
    PHY_EV_SYNC,       /**< a sync word matched and the payload is still arriving */
    PHY_EV_FRAME,      /**< a whole frame, CRC verified */
    PHY_EV_CRC         /**< a whole frame arrived and its CRC failed */
} phy_ev_kind_t;

typedef struct {
    phy_ev_kind_t kind;
    uint8_t  len;                    /**< payload bytes in buf, 0 unless kind is FRAME */
    uint8_t  buf[PHY_MAX_PAYLOAD];
    int16_t  rssi_dbm;               /**< this frame's own level, not the floor */
    uint32_t sync_us;                /**< the sync edge on phy_now_us's clock */
    uint8_t  sync_valid;             /**< 0 when no edge was timestamped for this frame */
} phy_ev_t;

/** @brief Brings the chip up on the protocol's PHY. */
int phy_init(void);

/** @brief Tunes the carrier; the caller names the channel, never this layer. */
int phy_tune(uint32_t hz);

/** @brief Enters continuous receive and stays there across frames and CRC failures. */
int phy_listen(void);

/** @brief Leaves receive without changing the configuration. */
int phy_standby(void);

/** @brief Transmits, blocking to the end of the frame, and returns to receive. */
int phy_transmit(const void *payload, uint8_t len, uint32_t *air_us);

/** @brief Non-blocking; fills ev with what the chip has, PHY_EV_NONE when nothing. */
int phy_poll(phy_ev_t *ev);

/** @brief The level with no frame on it, which is the floor the frames stand on. */
int16_t phy_rssi_now(void);

/** @brief One clock for every timestamp this interface reports, in microseconds. */
uint32_t phy_now_us(void);
