/* Microsecond timebase. TIM2 free-runs at 1 MHz over its full 32 bits, so the
 * counter wraps every ~71.6 minutes and every comparison here survives that. */
#include "main.h"
#include "timebase.h"

extern TIM_HandleTypeDef htim2;

void timebase_start(void) {
    HAL_TIM_Base_Start(&htim2);
}

uint32_t micros(void) {
    return __HAL_TIM_GET_COUNTER(&htim2);
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
