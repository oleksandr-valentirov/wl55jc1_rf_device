#pragma once

#include <stdint.h>

/* What this binary was built against, in one place.
 *
 * On a single core nothing here can disagree with itself, so the digests are a
 * **display**: they exist to be read out loud against the other board and
 * against the hub, which are built and flashed separately. Saying that plainly
 * matters - the hub's equivalent compares two cores and can fail on its own,
 * and treating these as the same kind of thing would be a name broader than
 * the coverage.
 *
 * The one part that can fail here is the last field: the locally generated hop
 * vectors against the hub's published set. */
typedef struct {
    const char *pair_v2;
    const char *wire_v3;
    const char *hop_shared;
    const char *hop_local;
    uint8_t     hop_local_matches_shared;
} vectors_report_t;

/** @brief Collects what this binary was built against, for reading aloud. */
void vectors_report(vectors_report_t *out);
