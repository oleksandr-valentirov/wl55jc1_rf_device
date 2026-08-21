#pragma once

#include <stdint.h>

/** @brief Starts TIM2 as the free-running microsecond counter. */
void     timebase_start(void);

/** @brief TIM2 now, in microseconds; wraps every 71.6 minutes. */
uint32_t micros(void);

/** @brief Milliseconds from the same counter, and wrapping with it. */
uint32_t millis_hw(void);

/** @brief Uptime in seconds, accumulated so it survives the TIM2 wrap.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
uint32_t timebase_uptime_s(void);

/** @brief Folds the accumulator; the superloop calls it so a wrap is not missed. */
void     timebase_service(void);

/** @brief Whether a deadline has passed, correct across the counter's wrap. */
uint8_t  timebase_elapsed(uint32_t deadline_us);

/** @brief Blocking delay, for waits too short to schedule around. */
void     delay_us_poll(uint32_t us);
