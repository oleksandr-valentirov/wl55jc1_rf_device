#pragma once

#include <stdint.h>

/**
 * @file device.h
 * @brief The protocol the node runs on its own, and a read-only view of it.
 *
 * Nothing here is armed, enabled or started by anything a person types. The
 * console is an observer of this module and never an input to it.
 *
 * radio_devices_docs/wl55_device/radio/pairing.md
 */

/** @brief Restores the grant from flash, provisions an identity if there is none. */
void device_init(void);

/**
 * @brief One pass of the node's own state machine; called every superloop pass.
 *
 * Unpaired it listens for an invitation, paired it recovers and reports. Which
 * of the two runs is decided here and nowhere else.
 */
void device_service(void);

/** @brief Drains one telemetry record to the port. */
void telemetry_service(void);

/** @brief What the node is doing, for an observer that may not change it. */
typedef struct device_view {
    uint8_t  paired;            /**< a grant survived the last reset, or was granted since */
    uint8_t  provisioned;       /**< flash holds an identity */
    uint32_t dev_id;
    uint32_t hub_id;
    uint16_t net_id;
    uint8_t  slot;              /**< base slot; the other opportunities derive from it */
    uint8_t  report_every;      /**< superframes between reports, as granted */
    uint8_t  key_gen;
    uint32_t superframe;        /**< the node's current count, 0 before the first beacon */
    uint32_t period_us;         /**< measured superframe period, 0 while unmeasured */
    uint32_t since_beacon_us;
    uint8_t  schedulable;       /**< the clock is fresh enough to place a slot */
    uint8_t  recovering;
    uint32_t recover_entered;
    int8_t   rssi_down_dbm;     /**< the hub's last beacon as this node heard it */
    uint8_t  rssi_valid;
    uint32_t tx_floor;          /**< superframe of the newest downlink opened this boot */
    uint8_t  tx_floor_known;    /**< nothing may be sealed until one has */
    uint32_t invites_heard;
    uint32_t invites_refused;
    uint32_t reports_sent;
    uint32_t beacons_missed;
    uint32_t downlinks_applied;
} device_view_t;

/**
 * @brief Fills a caller's view of the node. Reads only; changes nothing.
 * @param v where to put it
 */
void device_snapshot(device_view_t *v);

/**
 * @brief The node's public key, for the operator who has to enrol it on a hub.
 * @param pub    X25519_PUB_LEN bytes, the u-coordinate
 * @retval 0     written
 * @retval -1    no identity in flash
 */
int device_pubkey(uint8_t *pub);
