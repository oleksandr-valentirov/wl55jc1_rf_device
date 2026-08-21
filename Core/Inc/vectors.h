#pragma once

#include <stdint.h>

/* One build here, so these are a display; only the hop set can fail on its own.
 * radio_devices_docs/wl55_device/testing/console.md */
typedef struct {
    const char *pair_v2;
    const char *wire_v3;
    const char *hop_shared;
    const char *hop_local;
    uint8_t     hop_local_matches_shared;
} vectors_report_t;

/** @brief Collects what this binary was built against, for reading aloud. */
void vectors_report(vectors_report_t *out);
