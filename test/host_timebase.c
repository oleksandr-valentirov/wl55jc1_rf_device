/* A settable clock: the beacon rules at speed and at the counter wrap.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <stdint.h>

static uint32_t now_us;

uint32_t micros(void) { return now_us; }

/* All four, not the two this suite calls: a partial backend is what ADR-0029 refuses. */
uint32_t timebase_now(void) { return now_us; }
uint32_t timebase_us_to_ticks(uint32_t us) { return us; }
uint32_t timebase_ticks_to_us(uint32_t ticks) { return ticks; }

uint8_t timebase_elapsed(uint32_t deadline) {
    return (int32_t)(now_us - deadline) >= 0;
}

void host_clock_set(uint32_t us) { now_us = us; }
void host_clock_advance(uint32_t us) { now_us += us; }

/* Stubbed here, not #ifdef'd into load.c: the firmware carries no host-only branch.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
void load_enter(int bucket) { (void)bucket; }
void load_exit(int bucket) { (void)bucket; }
