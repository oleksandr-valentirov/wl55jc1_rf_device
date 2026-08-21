/* A settable clock, so the beacon rules can be tested at speed and at the
 * counter wrap - neither of which a bench run reaches. */
#include <stdint.h>

static uint32_t now_us;

uint32_t micros(void) { return now_us; }

uint8_t timebase_elapsed(uint32_t deadline) {
    return (int32_t)(now_us - deadline) >= 0;
}

void host_clock_set(uint32_t us) { now_us = us; }
void host_clock_advance(uint32_t us) { now_us += us; }

/* The CPU-time accounting is a device instrument; on the host it only has to
 * link. Stubbed here rather than #ifdef'd into load.c, so the firmware has no
 * host-only branch in it. */
void load_enter(int bucket) { (void)bucket; }
void load_exit(int bucket) { (void)bucket; }
