/* The quiesce rules at speed and at the wrap: every rule here is a refusal.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <stdio.h>
#include <string.h>

#include "beacon.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "superframe.h"
#include "clock.h"

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

/* Two beacons: the first is taken on trust, the second leaves the paired state.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
static void settle(superframe_t *sf, quiesce_t *q, uint32_t start) {
    memset(sf, 0, sizeof(*sf));
    memset(q, 0, sizeof(*q));
    host_clock_set(1000000u);
    superframe_start(sf, start, SUPERFRAME_US);
    uint8_t len = build(2u, start, 0u, 0u);
    beacon_apply(frame, len, sf, q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
    host_clock_advance(SUPERFRAME_US);
    len = build(2u, start + 1u, 0u, 0u);
    beacon_apply(frame, len, sf, q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
}

static void announce(superframe_t *sf, quiesce_t *q, uint32_t at, uint8_t resume_in) {
    uint8_t len = build(2u, at, RADIO_BEACON_FLAG_QUIESCE, resume_in);
    beacon_apply(frame, len, sf, q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
}

static void test_gates(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    uint8_t len = build(1u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_BAD_VERSION,
          "v1 must be rejected, not parsed as v2");
    len = build(3u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_BAD_VERSION,
          "a future version must be rejected rather than misparsed");

    len = build(2u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len - 1u, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_BAD_LENGTH,
          "v2 at the wrong length must be rejected");

    frame[0] = RADIO_FRAME_JOIN_BEACON;
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_NOT_BEACON,
          "a join beacon is not a data beacon");

    /* Version before length: a future layout that is 14 bytes must still be refused.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    len = build(3u, 102u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_BAD_VERSION,
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
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
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
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
    }
    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 108u + RADIO_QUIESCE_MIN_GAP, 4u);
    CHECK(q.active, "after the gap an honest quiesce must be honored again");
}

/* The first beacon is taken on trust, so a quiesce riding it went through no check.
 * radio_devices_docs/wl55_device/radio/beacon.md */
static void test_first_beacon_quiesce(void) {
    superframe_t sf; quiesce_t q;
    memset(&sf, 0, sizeof(sf));
    memset(&q, 0, sizeof(q));
    host_clock_set(1000000u);
    superframe_start(&sf, 0u, SUPERFRAME_US);

    announce(&sf, &q, 1000000u, 4u);
    CHECK(!q.active,
          "a quiesce on the very first beacon must not be honored - the counter "
          "it names went through no plausibility check");
}

/* The hub repeating the announcement is what stops a refusal costing a real quiesce.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
static void test_rejection_then_announce(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 100u);

    uint8_t len = build(2u, 100u + SUPERFRAME_MAX_JUMP + 10u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_SUSPECT, "setup");
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

/* A counter run ahead sees every honest beacon as an enormous forward jump.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
static void test_recovers_from_running_ahead(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 1000u);

    /* Hub at 1002, device at 1200 because a bad period estimate ran it forward.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    sf.g.counter = 1200u;

    int recovered = 0;
    for (int i = 0; i < 64; i++) {
        host_clock_advance(SUPERFRAME_US);
        uint8_t len = build(2u, 1002u + (uint32_t)i, 0u, 0u);
        if (beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_OK) {
            recovered = i + 1;
            break;
        }
    }
    CHECK(recovered, "a device that ran ahead never recovered - deaf until "
                     "power cycle, with the hub transmitting normally");
    /* 0 now: the signed jump test accepts from behind, so no escape is needed.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    if (recovered)
        printf("     recovered after %d refused beacon(s)\n", recovered - 1);
}

/* The two rules must compose: eight frames spent must not buy the ninth.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
static void test_resync_does_not_admit_quiesce(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 1000u);
    sf.g.counter = 1200u;

    /* Beyond SUPERFRAME_MAX_JUMP: merely behind is where honest beacons arrive.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    for (int i = 0; i < (int)SUPERFRAME_RESYNC_AFTER; i++) {
        host_clock_advance(SUPERFRAME_US);
        uint8_t len = build(2u, 1200u + SUPERFRAME_MAX_JUMP + 1000u + (uint32_t)i,
                            0u, 0u);
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
    }
    CHECK(!sf.aligned, "alignment must have been given up by now");

    host_clock_advance(SUPERFRAME_US);
    announce(&sf, &q, 1010u, 4u);
    CHECK(!q.active,
          "a quiesce on the trust beacon after a resync must be refused");
}

/* A missed beacon is not a longer superframe: frames comes from the beacon's field.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
static void test_missed_beacon_does_not_poison_period(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 500u);

    uint32_t before = sf.g.period;
    for (uint32_t i = 0; i < 6u; i++) {
        /* Two superframes of clock, and a counter that says so. */
        host_clock_advance(2u * SUPERFRAME_US);
        uint8_t len = build(2u, 502u + i * 2u, 0u, 0u);
        CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_OK,
              "a beacon after a quiesce gap must still be accepted");
    }
    CHECK(sf.g.period >= SUPERFRAME_PERIOD_MIN_US &&
          sf.g.period <= SUPERFRAME_PERIOD_MAX_US,
          "period drifted to %lu after gapped beacons, want %u..%u",
          (unsigned long)sf.g.period,
          (unsigned)SUPERFRAME_PERIOD_MIN_US, (unsigned)SUPERFRAME_PERIOD_MAX_US);
    (void)before;
}

/* The clamp is the bound on how far a lie can carry the estimate. */
static void test_period_clamp(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 700u);

    for (uint32_t i = 0; i < 12u; i++) {
        /* One superframe claimed while stalling for three walks the estimate up.
         * radio_devices_docs/wl55_device/testing/host-tests.md */
        host_clock_advance(3u * SUPERFRAME_US);
        uint8_t len = build(2u, 702u + i, 0u, 0u);
        beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
    }
    CHECK(sf.g.period <= SUPERFRAME_PERIOD_MAX_US,
          "a lying transmitter walked the period to %lu, above the %u ceiling",
          (unsigned long)sf.g.period, (unsigned)SUPERFRAME_PERIOD_MAX_US);
}

/* A hub reboot jumps by KV_RESERVE_AHEAD and must be accepted, not self-healed.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
static void test_hub_reboot_jump_is_accepted(void) {
    superframe_t sf; quiesce_t q;
    settle(&sf, &q, 5000u);

    host_clock_advance(SUPERFRAME_US);
    uint8_t len = build(2u, 5001u + 4096u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_OK,
          "a %u-superframe reserve jump must be accepted, not read as a forgery",
          4096u);
    CHECK(sf.g.counter == 5001u + 4096u, "counter %lu", (unsigned long)sf.g.counter);

    /* And the bound still bites well before a day's worth of counter space. */
    settle(&sf, &q, 5000u);
    host_clock_advance(SUPERFRAME_US);
    len = build(2u, 5001u + SUPERFRAME_MAX_JUMP + 1u, 0u, 0u);
    CHECK(beacon_apply(frame, len, &sf, &q, micros() - RADIO_AIR_START_TO_END_US(14u), NULL) == BEACON_SUSPECT,
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
    CHECK(sf.g.running, "align left the clock stopped");
    CHECK(sf.g.period != 0u, "a running clock kept a zero period");
    CHECK((int32_t)((sf.g.start + sf.g.period) - micros()) > 0,
          "the next boundary is not ahead of now, so it can never be reached");

    /* Gated, not sequenced: CHECK records and returns, so this would hang. */
    if (sf.g.period == 0u) {
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
    superframe_start(&sf, 100u, 0u);
    CHECK(sf.g.period != 0u, "superframe_start kept a zero period");
}

/* A key decides whether a device can join the grid; the counter mark does not.
 * radio_devices_docs/wl55_device/radio/timebase.md */
static void test_mark_ahead_does_not_block_sync(void) {
    superframe_t sf;
    quiesce_t q;
    const uint32_t mark = 720721u;   /* what node B's flash actually held */
    const uint32_t hub  = 720085u;   /* where the hub actually was */

    memset(&sf, 0, sizeof(sf));
    memset(&q, 0, sizeof(q));
    host_clock_set(1000000u);
    /* Seeded from the durable mark, which is the estimate a booted device has. */
    superframe_start(&sf, mark, SUPERFRAME_US);

    uint8_t len = build(2u, hub, 0u, 0u);
    int rc = beacon_apply(frame, len, &sf, &q,
                          micros() - RADIO_AIR_START_TO_END_US(14u), NULL);
    CHECK(rc == BEACON_OK, "a beacon below the durable mark was refused, rc %d", rc);
    CHECK(sf.g.counter == hub, "counter %lu, want the hub's %lu",
          (unsigned long)sf.g.counter, (unsigned long)hub);
    CHECK(sf.state != SF_SYNC_STALE, "state is STALE after a legitimate beacon");
    CHECK(sf.rejected == 0u, "%lu refusal(s) recorded", (unsigned long)sf.rejected);
}

int main(void) {
    test_mark_ahead_does_not_block_sync();
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
