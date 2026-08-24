/* The uptime fold, kept out of timebase.c so the host can test it.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#include "clock.h"

static uint32_t up_mark_us;
static uint32_t up_frac_us;
static uint32_t up_secs;

uint32_t timebase_uptime_s(void) {
    uint32_t now = micros();
    /* Unsigned, so the difference is right across the counter's wrap. */
    uint32_t elapsed = now - up_mark_us;

    up_mark_us = now;
    up_frac_us += elapsed;
    while (up_frac_us >= 1000000u) {
        up_frac_us -= 1000000u;
        up_secs++;
    }
    return up_secs;
}

void timebase_service(void) {
    (void)timebase_uptime_s();
}
