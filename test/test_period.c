/* The estimate's spread is what matters: it multiplies every scheduled offset.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "superframe.h"

void host_clock_set(uint32_t us);

static int failures;

static void ok(const char *what, long got, long limit) {
    if (labs(got) > limit) {
        printf("FAIL %-46s |%ld| > %ld\n", what, got, limit);
        failures++;
    } else {
        printf("  %-46s %+ld (limit %ld)\n", what, got, limit);
    }
}

/* Deterministic, so a failure is reproducible and not a bad afternoon. */
static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

/* The hub's true period against this clock: item 35's mean, 2381 ppm fast. */
#define TRUE_PER   2004762u
#define JITTER_US  600

/* Worst error of the estimate over a run, in microseconds. */
static long run(uint32_t seed, uint32_t beacons, uint32_t *n_est) {
    superframe_t sf;
    uint32_t at = 1000000u;
    long worst = 0;

    host_clock_set(at);
    superframe_start(&sf, 0u, SUPERFRAME_US, 0u);
    *n_est = 0;
    for (uint32_t i = 1; i <= beacons; i++) {
        uint32_t jitter = lcg(&seed) % (2u * JITTER_US + 1u);
        at += TRUE_PER;
        if (superframe_align_at(&sf, i, at - JITTER_US + jitter) != 0)
            continue;
        if (sf.measured_us == 0u)
            continue;
        (*n_est)++;
        long err = (long)sf.measured_us - (long)TRUE_PER;
        if (labs(err) > labs(worst))
            worst = err;
    }
    return worst;
}

int main(void) {
    uint32_t n;

    /* Absolute: a bound derived from the constant under test moves with it.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    const long bound = 20;

    printf("  baseline %u superframes, jitter +-%d us\n",
           (unsigned)SUPERFRAME_PERIOD_BASELINE, JITTER_US);

    for (uint32_t seed = 1; seed <= 5; seed++) {
        superframe_t sf;
        uint32_t at = 1000000u, s = seed;
        long worst = 0;
        uint32_t settled = 0;

        host_clock_set(at);
        superframe_start(&sf, 0u, SUPERFRAME_US, 0u);
        for (uint32_t i = 1; i <= 600u; i++) {
            uint32_t jitter = lcg(&s) % (2u * JITTER_US + 1u);
            at += TRUE_PER;
            if (superframe_align_at(&sf, i, at - JITTER_US + jitter) != 0)
                continue;
            /* Past any plausible bootstrap, so the window is not the constant's. */
            if (i < 200u)
                continue;
            settled++;
            long err = (long)sf.measured_us - (long)TRUE_PER;
            if (labs(err) > labs(worst))
                worst = err;
        }
        char label[64];
        snprintf(label, sizeof(label), "seed %u: worst error over %u estimates",
                 (unsigned)seed, (unsigned)settled);
        ok(label, worst, bound);
    }

    /* The control: one superframe wide, so it must not meet the bound.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    long boot = run(11u, 2u, &n);
    if (n == 0u || labs(boot) <= bound) {
        printf("FAIL the two-beacon bootstrap met the span's bound (%ld, n=%u)\n",
               boot, (unsigned)n);
        failures++;
    } else {
        printf("  %-46s %+ld (must exceed %ld)\n",
               "control: two-beacon bootstrap", boot, bound);
    }

    if (failures) {
        printf("\n%d period check(s) failed\n", failures);
        return 1;
    }
    printf("\nall period checks passed\n");
    return 0;
}
