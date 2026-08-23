#pragma once

#include <stdint.h>

/**
 * @file vcp.h
 * @brief The virtual COM port as a transport, shared by telemetry and the console.
 *
 * radio_devices_docs/wl55_device/testing/console.md
 */

/**
 * @brief Writes bytes to the port, blocking up to the timeout.
 * @param data    bytes to send
 * @param len     how many
 * @param timeout milliseconds before the write is abandoned
 */
void vcp_write(const uint8_t *data, uint16_t len, uint32_t timeout);
