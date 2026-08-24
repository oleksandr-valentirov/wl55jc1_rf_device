/**
 * @file test_hublogic.c
 * @brief The hub's enrolment scheduler on a PC, where its states can be reached.
 *
 * Every state of this machine used to be an air test: eleven exchanges, an
 * hour, a shared band, and a result another transmitter could void. Above
 * `phy.h` it needs no chip, so a radio that is not there can drive it, and a
 * device on the other side can be scripted to behave the way ADR-0026 says a
 * device behaves.
 *
 * **The one rule this exists to hold** is that the device holds its
 * confirmation to `RADIO_PAIR_CONF_REGION` past the *invitation*, not past the
 * response it just received. Eleven of eleven exchanges on the bench died there
 * and no counter on either board said why.
 *
 * radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdio.h>
#include <string.h>

#include "fake_phy.h"
#include "hublogic.h"
#include "radio_slots.h"

static int fails;
static unsigned checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* The superloop, as fast as the test needs and no faster than the air allows. */
#define STEP_US   500u

static void run_for(uint32_t us) {
    uint32_t end = fp.now_us + us;

    while ((int32_t)(fp.now_us - end) < 0) {
        hub_service();
        fp.now_us += STEP_US;
        fake_phy_tick();
    }
}

static void start(void) {
    fake_phy_reset();
    host_crypto_seed(0x2026u);
    hub_init();
}

/* One enrolment, end to end, with a device that keeps ADR-0026's timing. */
static void case_exchange_completes(void) {
    hub_view_t v;

    start();
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.inits_sent >= 1u);
    CHECK(fp.dev_saw_init >= 1u);
    CHECK(v.req_seen >= 1u);
    CHECK(v.rsp_sent >= 1u);
    CHECK(fp.dev_saw_rsp >= 1u);
    /* Named one at a time: "the device refused the response" is four outcomes. */
    CHECK(fp.dev_rsp_wrong_ids == 0u);
    CHECK(fp.dev_rsp_eph_static == 0u);
    CHECK(fp.dev_rsp_bad_point == 0u);
    CHECK(fp.dev_rsp_mismatch == 0u);
    /* The leg the bench lost eleven times out of eleven. */
    CHECK(v.conf_seen >= 1u);
    CHECK(v.conf_mismatch == 0u);
    CHECK(v.accept_sent >= 1u);
    CHECK(v.paired >= 1u);
    CHECK(fp.dev_saw_accept >= 1u);
    /* Nothing may reach the accept by timing out and starting over. */
    CHECK(v.ex_timeouts == 0u);
}

/* The confirmation is late by design, so the hub must still be listening. */
static void case_confirm_lands_in_its_region(void) {
    hub_view_t v;
    uint32_t held;

    start();
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.rsp_to_conf_us > 0u);
    /* Measured from the invitation, which is what the region is counted from. */
    held = v.rsp_to_conf_us + v.req_to_rsp_us + v.init_to_req_us;
    CHECK(held >= (uint32_t)RADIO_PAIR_CONF_REGION * SUPERFRAME_US);
    CHECK(held < (uint32_t)(RADIO_PAIR_CONF_REGION + 1u) * SUPERFRAME_US);
    CHECK(fp.missed_while_deaf == 0u);
}

/* Why ADR-0026 exists: 105 ms after the response is outside the window.
 * radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md */
static void case_immediate_confirm_is_lost(void) {
    hub_view_t v;

    start();
    fp.confirm_immediately = 1;
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.rsp_sent >= 1u);
    CHECK(v.conf_seen == 0u);
    CHECK(v.paired == 0u);
    CHECK(fp.missed_while_deaf >= 1u);
}

/* A device that proves a different secret is refused, and says so. */
static void case_wrong_confirm_is_refused(void) {
    hub_view_t v;

    start();
    fp.corrupt_confirm = 1;
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.conf_seen >= 1u);
    CHECK(v.conf_mismatch >= 1u);
    CHECK(v.paired == 0u);
    CHECK(v.accept_sent == 0u);
}

/* A silent device leaves the exchange to time out, not to hang. */
static void case_silent_device_times_out(void) {
    hub_view_t v;

    start();
    fp.deaf_device = 1;
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.inits_sent >= 1u);
    CHECK(v.req_seen == 0u);
    CHECK(v.paired == 0u);
    CHECK(v.ex_timeouts >= 1u);
    /* And it recovers: a timed-out exchange must not block the next invitation. */
    CHECK(v.inits_sent >= 2u);
}

/* The deadline is armed from the response, so the slack is the turnaround. */
static uint32_t margin_us;

static void case_late_tolerance_is_the_turnaround(void) {
    hub_view_t v;

    start();
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.paired >= 1u);
    margin_us = v.init_to_req_us + v.req_to_rsp_us;
    /* Positive by construction - the response cannot precede the invitation. */
    CHECK(margin_us > 0u);
    /* And thin: a fraction of the superframe the confirmation is held for. */
    CHECK(margin_us < SUPERFRAME_US / 4u);
}

/* A region late is heard and refused: conf_seen counts arrivals. */
static void case_confirm_one_region_late_is_refused(void) {
    hub_view_t v;

    start();
    fp.conf_region = RADIO_PAIR_CONF_REGION + 1u;
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.req_seen >= 1u);
    CHECK(v.rsp_sent >= 1u);
    CHECK(v.conf_seen >= 1u);
    CHECK(v.conf_no_exchange >= 1u);
    CHECK(v.paired == 0u);
    CHECK(v.ex_timeouts >= 1u);
}

/* The guard band, which is the only shape that reproduces the bench's.
 * radio_devices_docs/radio/tdma.md */
static uint32_t guard_extra_us;

static void case_confirm_in_the_guard_is_never_heard(void) {
    hub_view_t v;
    uint32_t nominal_off;

    /* One run to learn where the invitation put the exchange on the grid. */
    start();
    run_for(24u * SUPERFRAME_US);
    nominal_off = (fp.t_init_us - 1000u) % SUPERFRAME_US;
    CHECK(nominal_off < SUPERFRAME_US - RADIO_END_GUARD_US);

    /* Then place the confirmation two milliseconds inside the end guard. */
    guard_extra_us = (SUPERFRAME_US - RADIO_END_GUARD_US + 2000u) - nominal_off;
    start();
    fp.conf_extra_us = guard_extra_us;
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.req_seen >= 1u);
    CHECK(v.rsp_sent >= 1u);
    CHECK(v.conf_seen == 0u);
    CHECK(v.paired == 0u);
    CHECK(v.ex_timeouts >= 1u);
    CHECK(fp.missed_while_deaf >= 1u);

    /* A band, not a cliff: the next window hears it again. */
    start();
    fp.conf_extra_us = guard_extra_us + RADIO_END_GUARD_US +
                       RADIO_UPLINK_OFFSET_US + 20000u;
    run_for(24u * SUPERFRAME_US);
    hub_snapshot(&v);
    CHECK(v.conf_seen >= 1u);
}

/* The hub does not transmit into the region it owes a listen. ADR-0026 */
static void case_hub_is_silent_in_the_region_it_listens_in(void) {
    start();
    run_for(24u * SUPERFRAME_US);

    CHECK(fp.dev_saw_init >= 1u);
    CHECK(fp.beacons_in_exchange == 0u);
    CHECK(fp.missed_while_talking == 0u);
}

/* The grid runs whether or not anyone is enrolling. */
static void case_grid_runs(void) {
    hub_view_t v;

    start();
    run_for(8u * SUPERFRAME_US);
    hub_snapshot(&v);

    CHECK(v.superframes >= 7u);
    CHECK(v.windows >= 7u);
    CHECK(v.inits_sent >= 1u);
    /* Of four even frames in eight, the one in the region is silent. ADR-0026 */
    CHECK(v.beacons >= 2u);
    CHECK(fp.hz == RADIO_JOIN_HZ);
}

int main(void) {
    case_grid_runs();
    case_exchange_completes();
    case_confirm_lands_in_its_region();
    case_immediate_confirm_is_lost();
    case_wrong_confirm_is_refused();
    case_silent_device_times_out();
    case_late_tolerance_is_the_turnaround();
    case_confirm_one_region_late_is_refused();
    case_confirm_in_the_guard_is_never_heard();
    case_hub_is_silent_in_the_region_it_listens_in();

    /* An empty population is not a pass: a deleted case must show up here. */
    if (checks < 30u) {
        printf("FAIL only %u checks ran, which is fewer than this suite has\n", checks);
        fails++;
    }
    printf("hublogic: %s (%u checks, deadline slack %u us, "
           "guard band opens %u us past the region)\n",
           fails ? "FAIL" : "ok", checks, margin_us, guard_extra_us);
    return fails ? 1 : 0;
}
