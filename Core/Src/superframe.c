/* The protocol's clock on the device side.
 *
 * Free-runs off TIM2 between beacons and is realigned by every beacon heard, so
 * drift only accumulates across missed ones. The boundary steps by a fixed
 * amount from the previous boundary rather than from "now": measuring the period
 * from whenever the last piece of work finished turns processing time into
 * permanent drift. */
#include "superframe.h"
#include "timebase.h"

void superframe_start(superframe_t *sf, uint32_t counter, uint32_t period_us,
                      uint32_t floor) {
    sf->counter = (int32_t)(counter - floor) < 0 ? floor : counter;
    /* A zero period never reaches its own next boundary.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    sf->period_us = period_us ? period_us : SUPERFRAME_STUB_US;
    sf->next_boundary_us = micros() + period_us;
    sf->last_beacon_us = micros();
    sf->floor = floor;
    sf->running = 1;
    sf->aligned = 0;
    sf->state = SF_SYNC_NONE;
    sf->rejected = 0;
    sf->have_prev = 0;
    sf->measured_us = 0;
}

uint32_t superframe_now(superframe_t *sf) {
    if (!sf->running)
        return 0;
    /* The loop steps by the period, so a zero one never terminates. */
    if (sf->period_us == 0u)
        return sf->counter;
    /* Absolute stepping. A long stall crosses several boundaries and each one
     * counts, rather than being collapsed into a single late tick. */
    while (timebase_elapsed(sf->next_boundary_us)) {
        sf->counter++;
        sf->next_boundary_us += sf->period_us;
    }
    return sf->counter;
}

int superframe_align(superframe_t *sf, uint32_t counter) {
    return superframe_align_at(sf, counter, micros());
}

/* `at_us` is when the beacon's first bit arrived, not when the device finished
 * reading it. The beacon sits at offset 0, so a boundary taken at the end of
 * the frame is late by its whole air time - about 8 ms - and every offset
 * computed from it inherits that. Wide windows absorb it; the 25 ms downlink
 * region does not. Air time is known in advance, so it is corrected for rather
 * than absorbed, the same distinction radio_slots.h draws for lead and guard. */
int superframe_align_at(superframe_t *sf, uint32_t counter, uint32_t at_us) {
    /* The hub owns this clock, but it does not own this device's history. A
     * counter below the durable floor would reuse a GCM nonce, so following it
     * is worse than losing synchronisation. */
    if ((int32_t)(counter - sf->floor) < 0) {
        sf->state = SF_SYNC_STALE;
        sf->rejected++;
        sf->refused_counter = counter;
        sf->refused_jump = (int32_t)(counter - sf->counter);
        return -1;
    }
    /* Only once already aligned. A device that has just booted has no opinion
     * about where the counter should be and has to take the first one it hears;
     * one that has been following the hub does have an opinion, and a beacon
     * disagreeing with it by a day is a forgery, not a resynchronisation. */
    int32_t jump = (int32_t)(counter - sf->counter);
    /* Signed, and symmetric. Unsigned, a beacon one superframe behind read as
     * one 4294967295 ahead - and behind is where every honest beacon arrives
     * while the stub period runs 5% fast, so this refused the recovery it was
     * written to protect. Going backwards is the floor check's business above,
     * which is durable; this one only judges distance. */
    if (sf->aligned &&
        (jump > (int32_t)SUPERFRAME_MAX_JUMP ||
         jump < -(int32_t)SUPERFRAME_MAX_JUMP)) {
        sf->state = SF_SYNC_SUSPECT;
        sf->rejected++;
        sf->refused_counter = counter;
        sf->refused_jump = jump;
        /* Refusing every beacon in a row is not evidence that every beacon is a
         * forgery; past a point it is evidence that this device is the one that
         * is wrong. Drop our own alignment - and the period estimate that most
         * likely caused it - so the next beacon is taken on trust. This one is
         * still refused, so a forger needs one more frame than a bystander. */
        if (sf->rejected >= SUPERFRAME_RESYNC_AFTER) {
            sf->aligned = 0;
            sf->have_prev = 0;
            sf->measured_us = 0;
            sf->period_us = SUPERFRAME_STUB_US;
        }
        return -2;
    }
    uint32_t now = at_us;

    /* Learn the hub's real period from the gap between two beacons. Free-running
     * against a measured period rather than a nominal one is what keeps drift
     * small across the beacons a sleeping device misses. */
    if (sf->have_prev && (int32_t)(counter - sf->prev_counter) > 0) {
        uint32_t frames = counter - sf->prev_counter;
        uint32_t elapsed = now - sf->prev_beacon_us;
        uint32_t per = elapsed / frames;
        /* This only means anything if the hub transmits at a fixed offset
         * within the superframe. A beacon sent at an arbitrary moment gives
         * elapsed/frames of something that is not the period, so an estimate
         * far from the running one is treated as a bad sample rather than as
         * news - the first is taken on trust because there is nothing to
         * compare it against. */
        uint32_t ref = sf->measured_us ? sf->measured_us : per;
        uint32_t spread = (per > ref) ? (per - ref) : (ref - per);
        if (spread * 8u > ref)
            per = 0;
        if (per >= SUPERFRAME_PERIOD_MIN_US && per <= SUPERFRAME_PERIOD_MAX_US) {
            /* Averaged in rather than taken outright: one beacon heard late
             * because the console was busy should nudge the estimate, not
             * replace it. */
            sf->measured_us = sf->measured_us ? (sf->measured_us + per) / 2u : per;
            sf->period_us = sf->measured_us;
        }
    }
    sf->prev_beacon_us = now;
    sf->prev_counter = counter;
    sf->have_prev = 1;

    sf->counter = counter;
    /* Aligning starts the clock; an unstarted one still has a zero period. */
    if (sf->period_us == 0u)
        sf->period_us = SUPERFRAME_STUB_US;
    sf->next_boundary_us = now + sf->period_us;
    sf->last_beacon_us = now;
    sf->running = 1;
    sf->aligned = 1;
    sf->state = SF_SYNC_OK;
    sf->rejected = 0;
    return 0;
}

int superframe_is_fresh(const superframe_t *sf) {
    return sf->aligned && sf->state == SF_SYNC_OK &&
           superframe_since_beacon_us(sf) <= SUPERFRAME_FRESH_US;
}

/* The stub is deliberately not the nominal period, so a device that has never
 * measured one is running on a number chosen to be wrong. Transmitting under it
 * puts the frame on the wrong channel and seals it under the wrong nonce. */
int superframe_can_schedule(const superframe_t *sf) {
    return superframe_is_fresh(sf) && sf->measured_us != 0u;
}

const char *superframe_state_name(const superframe_t *sf) {
    switch (sf->state) {
    case SF_SYNC_OK:      return "aligned";
    case SF_SYNC_SUSPECT: return "suspect - a beacon claimed an implausible jump";
    case SF_SYNC_STALE:   return "stale - a beacon would reuse a counter; re-pair";
    default:              return "free-running, no beacon yet";
    }
}

uint32_t superframe_since_beacon_us(const superframe_t *sf) {
    return micros() - sf->last_beacon_us;
}
