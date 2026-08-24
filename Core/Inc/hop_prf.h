#pragma once

#include <stdint.h>

#include "hop.h"

/* The consumer's glue: the link layer declares a PRF, not crypto.h. ADR-0030.
 * radio_devices_docs/wl55_device/radio/hopping.md */

/** @brief One AES-128 ECB block through CRYP; @p ctx is the 16-byte key. */
int hop_prf_aes(void *ctx, const uint8_t in[16], uint8_t out[16]);

#if WL55_DEV_COMMANDS
/** @brief The accelerator's word swap modelled, so a sweep has a negative control.
 *  radio_devices_docs/wl55_device/radio/hopping.md */
int hop_prf_swap32(void *ctx, const uint8_t in[16], uint8_t out[16]);
#endif
