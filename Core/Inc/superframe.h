#pragma once

#include <stdint.h>

#include "radio_slots.h"

/* Deliberately not 2000 ms. The hub's own superframe is 1992.5 ms of real time
 * because its reference runs fast, and it will move again when the LSE
 * discipline lands - so anything that quietly depends on the nominal value has
 * to fail here, on the bench, rather than on air. */
#define SUPERFRAME_STUB_US  1900000u

/* A device that is already aligned refuses a jump larger than this. The data
 * beacon is cleartext and unauthenticated by necessity, so anyone can forge one
 * claiming any counter; without a bound, a single forged frame drags a device
 * arbitrarily far forward. One day at the hub's cadence is far more slack than
 * a device in range ever needs, and far less than an attacker wants. */
#define SUPERFRAME_MAX_JUMP  43200u

/* How long an alignment stays usable without a fresh beacon. Two superframes,
 * because on the deliberately-wrong stub period the counter gains 100 ms per
 * superframe and is unusable almost at once - and a measured period is only as
 * good as the beacon it was measured against. The point is not the number: it
 * is that "aligned" must expire, because the state otherwise reports the last
 * event rather than current validity and a device transmits into a guess. */
#define SUPERFRAME_FRESH_US  (2u * SUPERFRAME_US)

/* The hub does not advertise its period and should not: it is observable from
 * consecutive beacons, which costs no air time, needs no trust in the hub
 * telling the truth, and keeps working when its calibration moves. These bounds
 * stop an erratic or forged pair of beacons from destroying a good estimate. */
/* Consecutive refusals after which the device gives up its own alignment and
 * takes the next beacon on trust again.
 *
 * Without this a device whose counter has run ahead of the hub - one bad first
 * period estimate is enough, and that sample is taken on trust because there is
 * nothing to compare it against - reads every honest beacon as a move backwards,
 * which the unsigned subtract turns into an enormous forward jump. It then
 * refuses the hub forever and is deaf until someone power-cycles it.
 *
 * This is not a weakening: a reboot already re-takes the first beacon on trust,
 * so the reachable states are the same and only the operator's involvement
 * changes. What actually guards against nonce reuse is the durable floor, which
 * still applies to the beacon that follows. */
#define SUPERFRAME_RESYNC_AFTER  8u

/* Bounds on an accepted period sample, from the shared contract rather than a
 * wide guess. The old 1-4 s window was set when the period was something to be
 * discovered; the hub's reference now runs at -27 ppm against real time, so
 * SUPERFRAME_US is the truth and +/-1% is ~100x what two disciplined clocks
 * produce together.
 *
 * Clamping trusts the hub *less*, not more. Unclamped, anything that can put a
 * beacon on air - a hostile hub, or a bench transmitter on the same sync word -
 * can walk this estimate across the whole window and race the counter ahead of
 * the hub, which is the deaf state SUPERFRAME_RESYNC_AFTER exists to escape.
 * Clamped, a lie moves it 1%. The measurement is what stays; the constant only
 * bounds how far a lie can carry it. */
#define SUPERFRAME_PERIOD_MIN_US \
    (SUPERFRAME_US - SUPERFRAME_US / 100u * SUPERFRAME_PERIOD_TOL_PCT)
#define SUPERFRAME_PERIOD_MAX_US \
    (SUPERFRAME_US + SUPERFRAME_US / 100u * SUPERFRAME_PERIOD_TOL_PCT)

typedef enum {
    SF_SYNC_NONE = 0,      /* free-running, never heard a beacon */
    SF_SYNC_OK,            /* aligned to a beacon */
    SF_SYNC_SUSPECT,       /* a beacon asked for a jump too large to believe */
    SF_SYNC_STALE          /* a beacon would move the counter backwards */
} sf_sync_t;

typedef struct {
    uint32_t counter;
    uint32_t period_us;
    uint32_t next_boundary_us;
    uint32_t last_beacon_us;
    uint32_t floor;            /* never below what flash says has been used */
    uint8_t  running;
    uint8_t  aligned;
    sf_sync_t state;
    uint32_t rejected;      /* beacons refused since the last good one */
    /* A refusal that does not name the value it refused makes the reader derive
     * the direction, and direction is the whole defect this pair exposes. */
    uint32_t refused_counter;
    int32_t  refused_jump;  /* signed: behind and far ahead are not one case */
    uint32_t prev_beacon_us;
    uint32_t prev_counter;
    uint32_t measured_us;   /* 0 until two beacons have been seen */
    uint8_t  have_prev;
} superframe_t;

/** @brief Starts the clock; a zero period is substituted with the stub. */
void     superframe_start(superframe_t *sf, uint32_t counter, uint32_t period_us,
                          uint32_t floor);

/** @brief Steps the counter to now and returns it. */
uint32_t superframe_now(superframe_t *sf);

/** @brief Aligns to a beacon; -1 is below the durable floor, -2 too far to believe. */
int      superframe_align(superframe_t *sf, uint32_t counter);

/** @brief Aligns to a beacon that arrived at a known instant.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
int      superframe_align_at(superframe_t *sf, uint32_t counter,
                            uint32_t at_us);

/** @brief Names the sync state for the console. */
const char *superframe_state_name(const superframe_t *sf);

/** @brief Microseconds since the last accepted beacon. */
uint32_t superframe_since_beacon_us(const superframe_t *sf);

/** @brief Aligned, fresh, and on a measured period rather than the stub.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
int      superframe_can_schedule(const superframe_t *sf);

/** @brief Whether the last beacon is inside SUPERFRAME_FRESH_US. */
int      superframe_is_fresh(const superframe_t *sf);
