#pragma once

/**
 * @file fake_phy.h
 * @brief The state of the radio that is not there, so a test can drive it.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdint.h>

#include "phy.h"

typedef struct {
    uint32_t now_us;              /**< the clock phy_now_us returns; the test moves it */
    uint32_t hz;
    uint8_t  inited, listening;
    uint8_t  init_fails, tx_fails;
    uint8_t  deaf_device;         /**< the device hears nothing the hub transmits */
    uint8_t  corrupt_confirm;     /**< the device proves a different secret */
    uint8_t  confirm_immediately; /**< pre-ADR-0026 timing, as the control */
    uint8_t  conf_region;         /**< regions past the invitation the confirm waits */
    uint32_t conf_extra_us;       /**< delay past that, for finding the window's edge */

    uint32_t tx_count, delivered, missed_while_deaf, sync_seq;
    uint32_t missed_while_talking; /**< arrived inside the hub's own transmission */
    uint32_t beacons_in_exchange; /**< beacons the hub put into a region it owed a listen */
    uint8_t  exchange_open;
    uint8_t  last_tx_type;

    uint8_t  pending[PHY_MAX_PAYLOAD];
    uint8_t  pending_len;
    uint32_t pending_at;          /**< when the device's frame arrives */

    uint32_t dev_saw_init, dev_saw_rsp, dev_saw_accept, dev_rsp_mismatch;
    uint32_t dev_rsp_wrong_ids, dev_rsp_eph_static, dev_rsp_bad_point;
    uint32_t dev_rsp_derive_bad;
    uint32_t t_init_us, accept_at_us;
    uint32_t hub_id, dev_id, req_superframe;
    uint8_t  hub_static[32], dev_priv[32], dev_pub[32], dev_nonce[8];
} fake_phy_t;

extern fake_phy_t fp;

/** @brief Expires a frame nobody was listening for; one call per loop step. */
void fake_phy_tick(void);

/** @brief Puts the radio and the device back where they started. */
void fake_phy_reset(void);

/** @brief Reseeds the host's deterministic TRNG, so a failure can be re-run. */
void host_crypto_seed(uint32_t seed);
