#pragma once

#include <stdint.h>

/* One build here, so these are a display; only the hop set can fail on its own.
 * radio_devices_docs/wl55_device/testing/console.md */
typedef struct {
    const char *pair_v4;
    const char *wire_v4;
    const char *hop_shared;
    const char *hop_local;
    uint8_t     hop_local_matches_shared;
    int8_t      hop_deck_rc;   /**< 0 when this board's hop.c drew the published deck */
#if WL55_DEV_COMMANDS
    int8_t      hop_ref_rc;    /**< 0 when the hub's hop.c drew the same deck here */
    int8_t      hop_ref_ctl_rc;/**< non-zero required: a wrong PRF must be refused */
#endif
} vectors_report_t;

/** @brief Collects what this binary was built against, for reading aloud. */
void vectors_report(vectors_report_t *out);

#if WL55_DEV_COMMANDS
/** @brief The published hop key, so a sweep keys both implementations alike. */
const uint8_t *vectors_hop_key(void);
#endif
