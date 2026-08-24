/* TIM2 free-runs at 1 MHz over 32 bits; every comparison survives the wrap.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#include "main.h"
#include "clock.h"

extern TIM_HandleTypeDef htim2;

void timebase_start(void) {
    HAL_TIM_Base_Start(&htim2);
}

uint32_t micros(void) {
    return __HAL_TIM_GET_COUNTER(&htim2);
}

/* The seam's name for the counter this board already ran on. ADR-0029 decision 3. */
uint32_t timebase_now(void) {
    return micros();
}

/* The identity, and it must exist: nothing disciplines this TIM2. ADR-0029 decision 2.
 * radio_devices_docs/radio/decisions/0029-the-library-declares-four-backends-and-absorbs-no-control.md */
uint32_t timebase_us_to_ticks(uint32_t us) {
    return us;
}

uint32_t timebase_ticks_to_us(uint32_t ticks) {
    return ticks;
}

uint32_t millis_hw(void) {
    return micros() / 1000u;
}

/* The uptime fold lives in uptime.c: it needs no HAL, so the host can test
 * it across the wrap. */

/* Signed difference, so a deadline straddling the wrap still compares right. */
uint8_t timebase_elapsed(uint32_t deadline_us) {
    return (int32_t)(micros() - deadline_us) >= 0;
}

void delay_us_poll(uint32_t us) {
    const uint32_t end = micros() + us;
    while (!timebase_elapsed(end)) {
        __asm__("nop");
    }
}
