/**
 * @file phy_sx126x.c
 * @brief phy.h on this board's in-package radio; the hub's backend is the RFM69.
 *
 * radio_devices_docs/radio/phy-seam.md
 */
#include <string.h>

#include "phy.h"
#include "radio.h"
#include "timebase.h"

int phy_init(void) {
    /* The join slot, because the caller tunes before it transmits anything. */
    return radio_configure(RADIO_JOIN_SLOT);
}

int phy_tune(uint32_t hz) {
    return radio_set_carrier_hz(hz);
}

int phy_listen(void) {
    return radio_listening() ? 0 : radio_listen();
}

int phy_standby(void) {
    return radio_listen_stop();
}

int phy_transmit(const void *payload, uint8_t len, uint32_t *air_us) {
    int was = radio_listening();
    int rc;

    if (was)
        radio_listen_stop();
    rc = radio_send((const uint8_t *)payload, len, air_us);
    /* Back to listening immediately: standby loses the reply with no error. */
    if (was)
        radio_listen();
    return rc;
}

int phy_poll(phy_ev_t *ev) {
    radio_rx_info_t info;
    int rc;

    memset(ev, 0, sizeof(*ev));
    if (!radio_listening())
        return -1;
    rc = radio_listen_poll(ev->buf, sizeof(ev->buf), &info);
    if (rc == 1) {
        ev->kind = PHY_EV_NONE;
        return 0;
    }
    ev->sync_valid = info.capture_valid && info.capture_sync;
    ev->sync_us    = info.capture_us;
    if (rc == 0) {
        ev->kind     = PHY_EV_FRAME;
        ev->len      = info.len;
        ev->rssi_dbm = info.rssi_dbm;
        return 0;
    }
    if (rc == -3) {
        /* Reported, never dropped: a failed CRC is the population a link fault lives in. */
        ev->kind = PHY_EV_CRC;
        return 0;
    }
    if (rc == -5) {
        ev->kind = PHY_EV_SYNC;
        return 0;
    }
    return -1;
}

int16_t phy_rssi_now(void) {
    /* No instantaneous read on this part outside a packet; the floor is unmeasured. */
    return 0;
}

uint32_t phy_now_us(void) {
    return micros();
}
