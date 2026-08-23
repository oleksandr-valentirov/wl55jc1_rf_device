#pragma once

#include <stdint.h>

/**
 * @file sensor.h
 * @brief What the node measures about itself, off the ADC's two internal channels.
 *
 * radio_devices_docs/wl55_device/arch/sensors.md
 */

/** @brief One pair of readings, taken together so both share a supply figure. */
typedef struct sensor_reading {
    int16_t  temp_c_x10;    /**< the die, in tenths of a degree Celsius */
    uint16_t supply_mv;     /**< VDDA, from VREFINT against its factory value */
    uint32_t taken_ms;      /**< when, so a caller can tell a stalled reading */
    uint8_t  valid;
} sensor_reading_t;

/** @brief Takes a reading when one is due; called every superloop pass. */
void sensor_service(void);

/**
 * @brief The last reading taken.
 * @param out where to put it
 * @retval  0  a reading exists
 * @retval -1  nothing has been measured yet
 */
int sensor_read(sensor_reading_t *out);

/** @brief Conversions attempted and refused, for the console. */
void sensor_counts(uint32_t *taken, uint32_t *failed);
