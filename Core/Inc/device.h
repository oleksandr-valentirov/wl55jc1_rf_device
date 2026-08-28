#pragma once

#include <stdint.h>

/**
 * @file device.h
 * @brief The protocol the node runs on its own, and a read-only view of it.
 *
 * Nothing here is armed, enabled or started by anything a person types. The
 * console is an observer of this module and never an input to it - except under
 * WL55_DEV_COMMANDS, which is off in the product build and says so.
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
    uint8_t  opp;               /**< which single opportunity the loop uses, when not all */
    uint8_t  opp_all;           /**< the loop transmits on every k rather than one */
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
    uint32_t tx_floor;          /**< superframe of the first downlink opened this boot */
    uint8_t  tx_floor_known;    /**< nothing may be sealed until one has */
    uint32_t invite_slices;     /**< times the node looked, never a denominator */
    uint32_t enrol_left_s;      /**< seconds the enrolment window has left, 0 when shut */
    uint8_t  enrol_open;        /**< whether the node would answer an invitation now */
    uint32_t invites_seen;      /**< frames that parsed as a PAIR_INIT at all */
    uint32_t invites_refused;
    uint32_t reports_sent;
    uint32_t beacons_missed;
    uint32_t beacon_windows;    /**< beacon receives the report loop opened, the miss denominator */
    uint32_t downlinks_applied;
    uint32_t downlink_windows;  /**< downlink receives opened, the opened denominator */
    uint32_t downlinks_opened;  /**< every one that passed its tag, keepalives included */
    uint32_t downlinks_repeat;  /**< carried the command already held, so nothing was applied */
    uint32_t downlinks_replay;  /**< authenticated, and not newer than the durable floor */
    uint32_t downlink_floor;    /**< the floor itself, 0 while none has been established */
    uint16_t up_seq;            /**< uplinks attempted this boot, as the next frame will carry */
    uint8_t  app_len;           /**< bytes the last accepted RADIO_CMD_APP carried */
    uint8_t  app_any;           /**< whether one has arrived at all this boot */
    uint8_t  app[6];            /**< those bytes, held and never interpreted */
    uint8_t  app_witness;       /**< the sum this node acknowledged for them */
    uint32_t app_refused;       /**< commands whose app_len ran past the array, so unacked */
    int8_t   rssi_up_dbm;       /**< the level the hub measured, as RADIO_CMD_LINK returned it */
    uint8_t  rssi_up_valid;     /**< 0 until one has arrived; 0 dBm is a real reading */
} device_view_t;

/* Bounded by a physical act, and a release is one.
 * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
#define DEVICE_ENROL_WINDOW_MS  600000u

/**
 * @brief Drops the pairing, keeps the identity, and starts listening again.
 * @retval  0  released; the enrolment window reopens from now
 * @retval -1  the store refused and the node is still paired
 *
 * The only route that does not need a debug probe. ROADMAP item 58.
 */
/** @brief Reopens the enrolment window without touching the stored pairing.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
void device_reopen_enrol(void);

int device_release_pairing(void);

#if WL55_DEV_COMMANDS
/**
 * @brief Picks which of the k opportunities the report loop transmits in.
 * @param k    the opportunity, 0..RADIO_SLOT_OPPS-1; ignored when all is set
 * @param all  non-zero to transmit on every opportunity instead of one
 * @retval  0  taken
 * @retval -1  k is outside the grid
 *
 * An instrument and not a feature. The grant that would set this autonomously
 * does not exist - ROADMAP item 77 - and until it does, a delivery figure
 * measured without stating k is a figure with no regime.
 *
 * This is the one place the console is an input to this module, which is why it
 * is compiled out of the product build.
 */
int device_set_opportunities(uint8_t k, uint8_t all);
#endif

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
