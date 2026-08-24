#pragma once

#include <stdint.h>

/**
 * @file hublogic.h
 * @brief The hub's radio logic with no chip under it: grid, beacons, enrolment.
 *
 * radio_devices_docs/radio/phy-seam.md
 */

/** @brief Every stage of the exchange, so no step can fail as a silent zero. */
typedef struct {
    uint32_t superframes;        /**< grid advances since boot */
    uint32_t beacons;            /**< join beacons transmitted */
    uint32_t inits_sent;
    uint32_t init_tx_err;
    uint32_t windows;            /**< receive windows opened */
    uint32_t ev_sync;            /**< sync words matched, frame or not */
    uint32_t ev_crc;             /**< frames whose CRC failed */
    uint32_t ev_frame;           /**< frames whose CRC verified */
    uint32_t req_seen;
    uint32_t req_bad_frame;      /**< type, version or length */
    uint32_t req_bad_ids;
    uint32_t req_bad_nonce;      /**< an all-zero dev_nonce carries no freshness */
    uint32_t req_bad_point;
    uint32_t req_no_key;         /**< the ephemeral keygen or the TRNG refused */
    uint32_t req_no_keys;        /**< the schedule refused; no response goes out */
    uint32_t rsp_sent;
    uint32_t rsp_tx_err;
    uint32_t conf_seen;
    uint32_t conf_bad_frame;
    uint32_t conf_bad_ids;
    uint32_t conf_no_exchange;   /**< arrived with nothing in flight to match it */
    uint32_t conf_mismatch;      /**< the device proved a different secret */
    uint32_t accept_sent;
    uint32_t accept_tx_err;
    uint32_t paired;
    uint32_t ex_timeouts;
    uint8_t  other_type;         /**< a frame type byte, never a count of them */
    uint32_t other_frames;       /**< how many carried a type outside the exchange */
    uint32_t init_to_req_us;     /**< the hub's own view of the budget the device measures */
    uint32_t req_to_rsp_us;      /**< this hub's whole turnaround, request in to response out */
    uint32_t rsp_to_conf_us;
    uint32_t conf_to_accept_us;
    uint32_t frame_counter;      /**< the grid's own clock, which the frames carry */
    uint32_t stall_us;           /**< the service gap injected each pass; 0 is the clean run */
    int16_t  req_rssi_dbm;       /**< the level the last request arrived at, not a floor */
    int16_t  conf_rssi_dbm;      /**< the same for the confirmation, which is the frame at issue */
} hub_view_t;

/** @brief Draws the hub's static key and starts the grid. */
void hub_init(void);

/** @brief One pass of the hub's superloop; never blocks longer than one frame. */
void hub_service(void);

/** @brief Copies the counters out for the console. */
void hub_snapshot(hub_view_t *v);
