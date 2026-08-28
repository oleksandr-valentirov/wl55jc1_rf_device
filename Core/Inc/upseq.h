#pragma once

#include <stdint.h>

/**
 * @file upseq.h
 * @brief The uplink counter, separated from the loop so its arithmetic has a host arm.
 *
 * The value a frame carries and the value already spent are two numbers, and
 * run `2026-08-28-2` found them written as one: the loop sealed the spent count
 * into the frame and incremented afterwards, so every frame named the one
 * before it. Nothing caught it, because the only host arm pinned the field's
 * layout and no instrument tied it to a second counter.
 *
 * radio_devices_docs/radio/decisions/0037-the-application-payload-is-the-downlinks-and-the-frame-does-not-grow.md
 */
typedef struct upseq {
    uint16_t spent;             /**< frames sealed this boot; the value the last one carried */
} upseq_t;

/**
 * @brief The value the frame being built carries: the count including itself.
 * @param s  the counter
 * @return   1 for the first frame of a boot, so 0 on the wire is no report yet
 *
 * Reading this does not spend it. A refused seal must leave no gap, which is
 * why taking the value and spending it are two calls rather than one.
 */
static inline uint16_t upseq_pending(const upseq_t *s) {
    return (uint16_t)(s->spent + 1u);
}

/** @brief Spends the pending value, once the frame it went into is sealed. */
static inline void upseq_commit(upseq_t *s) {
    s->spent = (uint16_t)(s->spent + 1u);
}

/** @brief What has been sealed this boot, which is what the console reports. */
static inline uint16_t upseq_spent(const upseq_t *s) {
    return s->spent;
}
