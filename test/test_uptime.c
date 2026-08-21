/* The wrap crossing, which no bench run has reached: 4255 s is the most seen.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#include <stdint.h>
#include <stdio.h>

uint32_t timebase_uptime_s(void);
void     host_clock_set(uint32_t us);
void     host_clock_advance(uint32_t us);

static int failures;

static void eq(const char *what, uint32_t got, uint32_t want) {
    if (got != want) {
        printf("FAIL %-34s got %lu, want %lu\n", what,
               (unsigned long)got, (unsigned long)want);
        failures++;
    } else {
        printf("  %-34s %lu\n", what, (unsigned long)got);
    }
}

int main(void) {
    /* The fold counts from the first call, not from zero. */
    host_clock_set(0u);
    eq("first call is the origin", timebase_uptime_s(), 0u);

    host_clock_advance(999999u);
    eq("under a second does not tick", timebase_uptime_s(), 0u);
    host_clock_advance(1u);
    eq("the second lands exactly", timebase_uptime_s(), 1u);

    /* Up to the last whole second the counter can express: 4294 s. */
    host_clock_advance(4293000000u);
    eq("last second before the wrap", timebase_uptime_s(), 4294u);

    /* Past 2^32: micros() wraps and the unsigned difference carries it. */
    host_clock_advance(2000000u);
    eq("across the wrap, still climbing", timebase_uptime_s(), 4296u);

    /* The hub's roadmap asserts uptime dies at 4295 s. It does not. */
    host_clock_advance(3600000000u);
    eq("an hour past the wrap", timebase_uptime_s(), 7896u);
    host_clock_advance(3600000000u);
    eq("two hours past the wrap", timebase_uptime_s(), 11496u);

    /* The superloop calls the fold every pass; that is what this protects. */
    printf(failures ? "\n%d uptime check(s) failed\n" : "\nall uptime checks passed\n",
           failures);
    return failures ? 1 : 0;
}
