/* The protocol clock: free-runs off TIM2, realigned by every beacon heard.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#include "superframe.h"
#include "timebase.h"

void superframe_start(superframe_t *sf, uint32_t counter, uint32_t period_us) {
    sf->counter = counter;
    /* A zero period never reaches its own next boundary.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    sf->period_us = period_us ? period_us : SUPERFRAME_STUB_US;
    sf->next_boundary_us = timebase_now() + period_us;
    sf->last_beacon_us = timebase_now();
    sf->running = 1;
    sf->aligned = 0;
    sf->state = SF_SYNC_NONE;
    sf->rejected = 0;
    sf->have_prev = 0;
    sf->have_ref = 0;
    sf->measured_us = 0;
}

uint32_t superframe_now(superframe_t *sf) {
    if (!sf->running)
        return 0;
    /* The loop steps by the period, so a zero one never terminates. */
    if (sf->period_us == 0u)
        return sf->counter;
    /* Absolute: a long stall crosses several boundaries and each one counts.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    while (timebase_elapsed(sf->next_boundary_us)) {
        sf->counter++;
        sf->next_boundary_us += sf->period_us;
    }
    return sf->counter;
}

int superframe_align(superframe_t *sf, uint32_t counter) {
    return superframe_align_at(sf, counter, timebase_now());
}

/* at_us is the first bit, not the end: the frame's own 8 ms would be permanent lag.
 * radio_devices_docs/wl55_device/radio/timebase.md */
int superframe_align_at(superframe_t *sf, uint32_t counter, uint32_t at_us) {
    /* A booted device takes the first value; the mark gates sealing, not this.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    int32_t jump = (int32_t)(counter - sf->counter);
    /* Signed and symmetric: unsigned, one behind read as 4294967295 ahead.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    if (sf->aligned &&
        (jump > (int32_t)SUPERFRAME_MAX_JUMP ||
         jump < -(int32_t)SUPERFRAME_MAX_JUMP)) {
        sf->state = SF_SYNC_SUSPECT;
        sf->rejected++;
        sf->refused_counter = counter;
        sf->refused_jump = jump;
        /* A run of refusals is evidence about this device. Still refuse this one.
         * radio_devices_docs/wl55_device/radio/timebase.md */
        if (sf->rejected >= SUPERFRAME_RESYNC_AFTER) {
            sf->aligned = 0;
            sf->have_prev = 0;
            sf->have_ref = 0;
            sf->measured_us = 0;
            sf->period_us = SUPERFRAME_STUB_US;
        }
        return -2;
    }
    uint32_t now = at_us;

    /* Bootstrap only: two beacons, so scheduling can start before a span exists.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    if (sf->measured_us == 0u && sf->have_prev &&
        (int32_t)(counter - sf->prev_counter) > 0) {
        uint32_t frames = counter - sf->prev_counter;
        uint32_t per = (now - sf->prev_beacon_us) / frames;
        if (per >= SUPERFRAME_PERIOD_MIN_US && per <= SUPERFRAME_PERIOD_MAX_US) {
            sf->measured_us = per;
            sf->period_us = per;
        }
    }
    /* The span, not the last pair: the noise scales the offsets it multiplies.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    if (!sf->have_ref) {
        sf->ref_beacon_us = now;
        sf->ref_counter = counter;
        sf->have_ref = 1;
    } else if ((int32_t)(counter - sf->ref_counter) >=
               (int32_t)SUPERFRAME_PERIOD_BASELINE) {
        uint32_t frames = counter - sf->ref_counter;
        uint32_t per = (now - sf->ref_beacon_us) / frames;
        if (frames <= SUPERFRAME_PERIOD_BASELINE_MAX &&
            per >= SUPERFRAME_PERIOD_MIN_US && per <= SUPERFRAME_PERIOD_MAX_US) {
            sf->measured_us = per;
            sf->period_us = per;
        }
        sf->ref_beacon_us = now;
        sf->ref_counter = counter;
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

/* Deliberately not the nominal period: a never-measured device is knowably wrong.
 * radio_devices_docs/wl55_device/radio/timebase.md */
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
    return timebase_now() - sf->last_beacon_us;
}
