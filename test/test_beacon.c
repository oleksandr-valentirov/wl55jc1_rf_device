/* The quiesce rules, exercised at speed and at the counter wrap.
 *
 * Every rule here is a refusal, and a refusal only runs when something is
 * wrong - which on a bench is never. The two ordering bugs this file was
 * written to catch were both found before it ran, which is the argument for
 * having it: they were found by reading, and reading does not scale. */
#include <stdio.h>
#include <string.h>

#include "beacon.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "superframe.h"
#include "timebase.h"

void host_clock_set(uint32_t us);
void host_clock_advance(uint32_t us);

static int failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("FAIL %s:%d  ", __func__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

static uint8_t frame[32];

static uint8_t build(uint8_t version, uint32_t sf, uint8_t flags, uint8_t resume_in) {
    radio_data_beacon_t b;
    b.type = RADIO_FRAME_DATA_BEACON;
    b.version = version;
    b.net_id = 1u;
    b.hub_id = 0xA7C31E55u;
    b.superframe = sf;
    b.flags = flags;
    b.resume_in = resume_in;
    memcpy(frame, &b, sizeof(b));
    return (uint8_t)sizeof(b);
}

/* A device already following the hub. Two beacons, because the first is taken
 * on trust and only the second leaves it in the state a paired device is in. */
static void settle(superframe_t *sf, quiesce_t *q, uint32_t start) {
    memset(sf, 0, sizeof(*sf));
    memset(q, 0, sizeof(*q));
    host_clock_set(1000000u);
    superframe_start(sf, start, SUPERFRAME_US, start);
    uint8_t len = build(2u, start, 0u, 0u);
    beacon_apply(frame, len, sf, q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
    host_clock_advance(SUPERFRAME_US);
    len = build(2u, start + 1u, 0u, 0u);
    beacon_apply(frame, len, sf, q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
}

static void announce(superframe_t *sf, quiesce_t *q, uint32_t at, uint8_t resume_in) {
    uint8_t len = build(2u, at, RADIO_BEACON_FLAG_QUIESCE, resume_in);
    beacon_apply(frame, len, sf, q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
}

static void test_gates(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    uint8_t len = build(1u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_BAD_VERSION,
          "v1 must be rejected, not parsed as v2");
    len = build(3u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_BAD_VERSION,
          "a future version must be rejected rather than misparsed");

    len = build(2u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len - 1u, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_BAD_LENGTH,
          "v2 at the wrong length must be rejected");

    frame[0] = RADIO_FRAME_JOIN_BEACON;
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_NOT_BEACON,
          "a join beacon is not a data beacon");

    /* Version is checked before length on purpose: a future layout that happens
     * to be 14 bytes must still be rejected as a version, not accepted. */
    len = build(3u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_BAD_VERSION,
          "version must be checked before length");
}

static void test_countdown(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    /* The announce run: same absolute resume superframe from every copy. */
    announce(&sf, &q, 102u, 4u);
    CHECK(q.active, "the announcement must take effect");
    CHECK(q.resume_at == 106u, "resume_at %lu, want 106", (unsigned long)q.resume_at);

    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 103u, 3u);
    CHECK(q.resume_at == 106u,
          "the second copy must name the same superframe, got %lu",
          (unsigned long)q.resume_at);

    CHECK(quiesce_active(&q, 105u), "still quiesced one superframe before resume");
    CHECK(!quiesce_active(&q, 106u), "resume superframe is not quiesced");
}

static void test_clamp(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    announce(&sf, &q, 102u, 200u);
    CHECK(q.clamped == 1u, "an over-long resume_in must be counted, not just fixed");
    CHECK(q.resume_at == 102u + RADIO_QUIESCE_SUPERFRAMES,
          "resume_at %lu, want %u", (unsigned long)q.resume_at,
          102u + RADIO_QUIESCE_SUPERFRAMES);
}

static void test_never_extends(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    announce(&sf, &q, 102u, 2u);            /* resume at 104 */
    CHECK(q.resume_at == 104u, "resume_at %lu, want 104", (unsigned long)q.resume_at);

    /* The hub commits and never extends, so this is a bug or a forgery. */
    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 103u, 4u);            /* would be 107 */
    CHECK(q.resume_at == 104u,
          "an announcement must not push resume out, got %lu",
          (unsigned long)q.resume_at);

    /* Forward is fine: it can only shorten the sleep. */
    announce(&sf, &q, 103u, 0u);            /* 103 */
    CHECK(q.resume_at == 103u,
          "an announcement may bring resume forward, got %lu",
          (unsigned long)q.resume_at);
}

static void test_rate_limit(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    announce(&sf, &q, 102u, 4u);            /* resume at 106 */
    CHECK(q.resume_at == 106u, "setup");

    /* Walk the clock past the resume so the quiesce retires. */
    for (uint32_t c = 102u; c <= 106u; c++) {
        host_clock_advance(SUPERFRAME_US);
        uint8_t len = build(2u, c, 0u, 0u);
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
    }
    CHECK(!q.active, "the quiesce must retire once its resume superframe passes");

    /* Immediately re-announcing is the replay: every copy inside spec, the
     * device asleep forever. */
    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 107u, 4u);
    CHECK(!q.active, "a fresh quiesce arriving too soon must be refused");
    CHECK(q.refused_gap == 1u, "the refusal must be counted");

    /* After the gap, an honest one is honored again. */
    for (uint32_t c = 108u; c < 108u + RADIO_QUIESCE_MIN_GAP; c++) {
        host_clock_advance(SUPERFRAME_US);
        uint8_t len = build(2u, c, 0u, 0u);
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
    }
    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 108u + RADIO_QUIESCE_MIN_GAP, 4u);
    CHECK(q.active, "after the gap an honest quiesce must be honored again");
}

/* The first beacon a device hears is taken on trust: superframe_align skips the
 * jump test when it has never been aligned. A quiesce riding on that beacon is
 * therefore a number nothing checked. */
static void test_first_beacon_quiesce(void) {
    superframe_t sf; quiesce_t q;
    memset(&sf, 0, sizeof(sf));
    memset(&q, 0, sizeof(q));
    host_clock_set(1000000u);
    superframe_start(&sf, 0u, SUPERFRAME_US, 0u);

    announce(&sf, &q, 1000000u, 4u);
    CHECK(!q.active,
          "a quiesce on the very first beacon must not be honored - the counter "
          "it names went through no plausibility check");
}

/* The two mechanisms interact, and in the right direction: a device that has
 * just refused a forgery ignores the next announcement, and the hub repeating
 * the announcement is what stops that from costing a legitimate quiesce. */
static void test_rejection_then_announce(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    uint8_t len = build(2u, 100u + SUPERFRAME_MAX_JUMP + 10u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_SUSPECT, "setup");
    CHECK(sf.rejected == 1u, "the forgery must be counted");

    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 102u, 4u);
    CHECK(!q.active, "an announcement right after a forgery must be ignored");
    CHECK(q.refused_sync == 1u, "the refusal must be counted");

    /* The second copy of the announce run lands on a clean slate. */
    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 103u, 3u);
    CHECK(q.active, "the repeated announcement must be honored");
    CHECK(q.resume_at == 106u, "resume_at %lu, want 106", (unsigned long)q.resume_at);
}

static void test_wrap(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 0xFFFFFFF0u);

    announce(&sf, &q, 0xFFFFFFFEu, 4u);     /* resume_at wraps to 2 */
    CHECK(q.resume_at == 2u, "resume_at %lu, want 2", (unsigned long)q.resume_at);
    CHECK(quiesce_active(&q, 0xFFFFFFFFu), "quiesced across the wrap");
    CHECK(quiesce_active(&q, 1u), "still quiesced just after the wrap");
    CHECK(!quiesce_active(&q, 2u), "resume superframe is not quiesced");
}

/* A device whose counter has run ahead of the hub sees every honest beacon as a
 * move backwards, which the unsigned subtract turns into an enormous forward
 * jump. If nothing clears that, the device is deaf until someone power-cycles
 * it - the failure shape the key-lifecycle doc calls the worst kind of safe. */
static void test_recovers_from_running_ahead(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 1000u);

    /* The hub is at 1002 and keeps beaconing; this device thinks it is at 1200
     * because a bad period estimate ran it forward. */
    sf.counter = 1200u;

    int recovered = 0;
    for (int i = 0; i < 64; i++) {
        host_clock_advance(SUPERFRAME_US);
        uint8_t len = build(2u, 1002u + (uint32_t)i, 0u, 0u);
        if (beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_OK) {
            recovered = i + 1;
            break;
        }
    }
    CHECK(recovered, "a device that ran ahead never recovered - deaf until "
                     "power cycle, with the hub transmitting normally");
    /* 0 is the expected answer now and is the stronger one: the signed jump
     * test accepts a beacon from behind outright, so recovery no longer has to
     * go through the eight-refusal escape at all. Printed rather than asserted
     * as a number, because it is a cost and not a contract. */
    if (recovered)
        printf("     recovered after %d refused beacon(s)\n", recovered - 1);
}

/* Forcing a resync must not be a way in. The trust beacon that follows one is
 * exactly the case the was-aligned rule exists for, so the two rules have to
 * compose: an attacker who spends eight frames to clear the alignment still
 * cannot put the device to sleep with the ninth. */
static void test_resync_does_not_admit_quiesce(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 1000u);
    sf.counter = 1200u;

    /* Beyond SUPERFRAME_MAX_JUMP, because a beacon merely *behind* is no longer
     * refused - the plausibility test is signed and symmetric now, and being
     * behind is where every honest beacon arrives while the stub period runs
     * fast. Forcing a resync therefore costs a real forgery, which is what this
     * test needs an attacker to have spent. */
    for (int i = 0; i < (int)SUPERFRAME_RESYNC_AFTER; i++) {
        host_clock_advance(SUPERFRAME_US);
        uint8_t len = build(2u, 1200u + SUPERFRAME_MAX_JUMP + 1000u + (uint32_t)i,
                            0u, 0u);
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
    }
    CHECK(!sf.aligned, "alignment must have been given up by now");

    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 1010u, 4u);
    CHECK(!q.active,
          "a quiesce on the trust beacon after a resync must be refused");
}

/* A missed beacon is not a longer superframe.
 *
 * The hub omits beacons for two superframes on every pairing quiesce, and
 * interference does the same by accident, so "consecutive beacons" and
 * "consecutive superframes" are different things by design. The period estimate
 * survives that only because `frames` comes from the beacon's superframe field
 * rather than from how many beacons were heard - at two superframes the naive
 * version yields 4.0 s, which is a 2x error averaged straight into the estimate. */
static void test_missed_beacon_does_not_poison_period(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 500u);

    uint32_t before = sf.period_us;
    for (uint32_t i = 0; i < 6u; i++) {
        /* Two superframes of clock, and a counter that says so. */
        host_clock_advance(2u * SUPERFRAME_US);
        uint8_t len = build(2u, 502u + i * 2u, 0u, 0u);
        CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_OK,
              "a beacon after a quiesce gap must still be accepted");
    }
    CHECK(sf.period_us >= SUPERFRAME_PERIOD_MIN_US &&
          sf.period_us <= SUPERFRAME_PERIOD_MAX_US,
          "period drifted to %lu after gapped beacons, want %u..%u",
          (unsigned long)sf.period_us,
          (unsigned)SUPERFRAME_PERIOD_MIN_US, (unsigned)SUPERFRAME_PERIOD_MAX_US);
    (void)before;
}

/* The clamp is the bound on how far a lie can carry the estimate. */
static void test_period_clamp(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 700u);

    for (uint32_t i = 0; i < 12u; i++) {
        /* A transmitter claiming one superframe passed while stalling for three.
         * Unclamped this walks the estimate up and races the counter. */
        host_clock_advance(3u * SUPERFRAME_US);
        uint8_t len = build(2u, 702u + i, 0u, 0u);
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL);
    }
    CHECK(sf.period_us <= SUPERFRAME_PERIOD_MAX_US,
          "a lying transmitter walked the period to %lu, above the %u ceiling",
          (unsigned long)sf.period_us, (unsigned)SUPERFRAME_PERIOD_MAX_US);
}

/* A hub reboot advances its counter by KV_RESERVE_AHEAD, because the store
 * persists a ceiling rather than the value. Devices see that as a forward jump
 * of up to 4096 superframes - ~2.3 h of counter space - from a hub that is
 * behaving correctly. It has to be accepted, or every hub reboot costs the whole
 * network the eight-beacon self-heal. */
static void test_hub_reboot_jump_is_accepted(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 5000u);

    host_clock_advance(SUPERFRAME_US);
    uint8_t len = build(2u, 5001u + 4096u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_OK,
          "a %u-superframe reserve jump must be accepted, not read as a forgery",
          4096u);
    CHECK(sf.counter == 5001u + 4096u, "counter %lu", (unsigned long)sf.counter);

    /* And the bound still bites well before a day's worth of counter space. */
    settle(&sf, &q, 5000u);
    host_clock_advance(SUPERFRAME_US);
    len = build(2u, 5001u + SUPERFRAME_MAX_JUMP + 1u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_FRAME_AIR_US(14u), NULL) == BEACON_SUSPECT,
          "beyond SUPERFRAME_MAX_JUMP must still be refused");
}

/* Aligning a clock nobody started, which wedged the core.
 * radio_devices_docs/wl55_device/radio/timebase.md */
static void test_align_without_start(void) {
    superframe_t sf;
    quiesce_t q;

    memset(&sf, 0, sizeof(sf));
    memset(&q, 0, sizeof(q));
    host_clock_set(1000000u);

    uint8_t len = build(2u, 555229u, 0u, 0u);
    beacon_rc_t rc = beacon_apply(frame, len, &sf, &q, micros(), NULL);
    CHECK(rc == BEACON_OK, "an unstarted clock refused a good beacon: %d", (int)rc);
    CHECK(sf.running, "align left the clock stopped");
    CHECK(sf.period_us != 0u, "a running clock kept a zero period");
    CHECK((int32_t)(sf.next_boundary_us - micros()) > 0,
          "the next boundary is not ahead of now, so it can never be reached");

    /* Gated, not sequenced: CHECK records and returns, so this would hang. */
    if (sf.period_us == 0u) {
        printf("  (skipping superframe_now: a zero period never returns)\n");
        return;
    }
    host_clock_advance(4u * SUPERFRAME_US);
    uint32_t n = superframe_now(&sf);
    CHECK(n >= 555229u && n < 555229u + 16u, "counter ran away to %lu",
          (unsigned long)n);
}

/* The same hole one layer down: nothing stops a caller passing zero. */
static void test_start_refuses_a_zero_period(void) {
    superframe_t sf;

    memset(&sf, 0, sizeof(sf));
    host_clock_set(1000000u);
    superframe_start(&sf, 100u, 0u, 100u);
    CHECK(sf.period_us != 0u, "superframe_start kept a zero period");
}

int main(void) {
    test_align_without_start();
    test_start_refuses_a_zero_period();
    test_gates();
    test_countdown();
    test_clamp();
    test_never_extends();
    test_rate_limit();
    test_first_beacon_quiesce();
    test_rejection_then_announce();
    test_recovers_from_running_ahead();
    test_resync_does_not_admit_quiesce();
    test_missed_beacon_does_not_poison_period();
    test_period_clamp();
    test_hub_reboot_jump_is_accepted();
    test_wrap();

    printf(failures ? "%d check(s) failed\n" : "all beacon checks passed\n", failures);
    return failures ? 1 : 0;
}
