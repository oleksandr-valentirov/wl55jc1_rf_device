/**
 * @file test_upseq.c
 * @brief The uplink counter's arithmetic, tied to a second counter rather than to a layout.
 *
 * `test_link` pins where `up_seq` sits in the frame and three mutations were
 * made red against that. None of them could see the value being wrong, because
 * a wrong count lays out exactly like a right one. Run `2026-08-28-2` found the
 * loop sealing the spent count instead of the pending one, on the air, after it
 * shipped - so what is checked here is the relation between what a frame
 * carried and how many frames there have been.
 *
 * What this cannot see is `device.c` open-coding the arithmetic again: the loop
 * is not host-compilable, so the tie is that it calls these two functions. The
 * end-to-end instrument stays RG-T-5, on air, against the hub's own count.
 *
 * radio_devices_docs/wl55_device/testing/host-tests.md
 */
#include <stdio.h>

#include "upseq.h"

static int fails;
static unsigned checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* One report cycle, as report_service runs it. */
typedef struct outcome {
    uint8_t seal_ok;
    uint8_t send_ok;
} outcome_t;

/* What the loop and the console hold between them. */
typedef struct run {
    uint16_t last_sealed;       /**< the value the last frame that flew carried */
    uint16_t reports;           /**< reports_sent: frames the radio accepted */
    uint16_t spent;             /**< upseq_spent, which is the console's up_seq */
} run_t;

/**
 * @brief Drives the loop over a script of outcomes.
 * @param before  non-zero to seal the spent count, which is the defect
 *
 * The defect is a parameter so the relation below is shown to refuse. A check
 * that has never refused reads in neither direction.
 */
static run_t drive(const outcome_t *script, unsigned n, int before) {
    upseq_t s = {0};
    run_t r = {0, 0, 0};

    for (unsigned i = 0; i < n; i++) {
        uint16_t carried = before ? upseq_spent(&s) : upseq_pending(&s);

        if (!script[i].seal_ok)
            continue;
        upseq_commit(&s);
        if (!script[i].send_ok)
            continue;
        r.last_sealed = carried;
        r.reports++;
    }
    r.spent = upseq_spent(&s);
    return r;
}

int main(void) {
    upseq_t s = {0};
    static const outcome_t clean[6] = {
        {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}
    };
    /* A refused seal, then a refused radio: the two ways a cycle ends early. */
    static const outcome_t rough[6] = {
        {1, 1}, {0, 0}, {1, 1}, {1, 0}, {1, 1}, {0, 0}
    };
    run_t r;

    /* Nothing sealed, so nothing may have been carried: the sentinel's whole basis. */
    CHECK(upseq_spent(&s) == 0u);
    CHECK(upseq_pending(&s) != 0u);

    /* Reading the pending value is not spending it. */
    CHECK(upseq_pending(&s) == upseq_pending(&s));
    CHECK(upseq_spent(&s) == 0u);

    upseq_commit(&s);
    CHECK(upseq_spent(&s) == 1u);
    CHECK(upseq_pending(&s) == 2u);

    /* Six clean cycles: the last frame said six and the console agrees. */
    r = drive(clean, 6u, 0);
    CHECK(r.reports == 6u);
    CHECK(r.last_sealed == r.reports);
    CHECK(r.spent == r.reports);

    /* The same script the defect's way, which is what shipped. */
    r = drive(clean, 6u, 1);
    CHECK(r.reports == 6u);
    CHECK(r.last_sealed != r.reports);
    CHECK(r.last_sealed == 5u);

    /* A refused seal spends nothing; a refused radio spends the nonce. */
    r = drive(rough, 6u, 0);
    CHECK(r.reports == 3u);
    CHECK(r.spent == 4u);
    CHECK(r.last_sealed == 4u);
    CHECK(r.last_sealed >= r.reports);

    /* The first frame of a boot carries 1, so the hub can tell it from silence. */
    r = drive(clean, 1u, 0);
    CHECK(r.last_sealed == 1u);
    r = drive(clean, 1u, 1);
    CHECK(r.last_sealed == 0u);

    /* The sentinel has one exception and it is stated, not discovered. */
    s.spent = 65534u;
    CHECK(upseq_pending(&s) == 65535u);
    upseq_commit(&s);
    CHECK(upseq_pending(&s) == 0u);
    upseq_commit(&s);
    CHECK(upseq_spent(&s) == 0u);

    if (fails) {
        printf("\n%d of %u upseq check(s) failed\n", fails, checks);
        return 1;
    }
    printf("upseq: ok (%u checks)\n", checks);
    return 0;
}
