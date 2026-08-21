#pragma once

#include <stdint.h>

/** @brief Brings up the console on the BSP's LPUART1. */
void CLI_Init(void);
/** @brief Drains typed input; called every superloop pass. */
void CLI_Poll(void);
/** @brief The granted uplink cadence: aligns on a beacon, then transmits.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
void report_service(void);
/** @brief Park-and-wait recovery after a lost counter.
 *  radio_devices_docs/radio/joining.md */
void recover_service(void);
/** @brief Drains one telemetry record per superloop pass. */
void telemetry_service(void);
/** @brief Feeds one received byte in from the UART interrupt. */
void CLI_RxByte(uint8_t b);
