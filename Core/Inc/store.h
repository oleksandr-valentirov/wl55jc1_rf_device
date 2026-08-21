#pragma once

#include <stdint.h>

#define STORE_PRIV_LEN 32
#define STORE_KEY_LEN   16
#define STORE_PUB_C_LEN 33   /* compressed SEC1 */

/* The superframe counter feeds the GCM nonce, so it must never go backwards
 * across a reset: a repeated counter under a surviving key is nonce reuse, and
 * GCM leaks its authentication subkey rather than merely a plaintext. Persisting
 * every superframe would wear the page out, so what is stored is a mark ahead of
 * the counter and only values below it are ever used. */
#ifndef STORE_COUNTER_STEP
#define STORE_COUNTER_STEP 1000u
#endif

typedef struct {
    uint8_t  priv[STORE_PRIV_LEN];
    uint32_t dev_id;
    uint32_t counter_mark;
    uint32_t key_gen;       /* which key generation rx_floor was written under */
    uint32_t rx_floor;
    uint32_t init_ceiling;
    uint32_t hub_id;        /* 0 until a join beacon has named the network */
    uint16_t net_id;
    uint8_t  session[STORE_KEY_LEN];
    uint8_t  hop_key[STORE_KEY_LEN];
    uint8_t  hub_static[STORE_PUB_C_LEN];
    uint8_t  slot;
    uint8_t  report_every;
    uint8_t  valid;
} store_state_t;

/** @brief Scans both pages and caches the newest valid record. */
int  store_init(store_state_t *out);

/** @brief Persists the identity; it cannot be re-derived once lost. */
int  store_save_identity(const uint8_t *priv, uint32_t dev_id);

/** @brief Persists the session key and clears the replay floor in one record.
 *  radio_devices_docs/wl55_device/arch/store.md */
int  store_save_session(const uint8_t *session);

/** @brief Persists the whole grant, which outlives the exchange that made it. */
int  store_save_pairing(const uint8_t *session, const uint8_t *hop_key,
                        uint8_t slot, uint8_t report_every);
/** @brief Persists a rate commanded after pairing; an unchanged rate does not write. */
int  store_save_report_every(uint8_t report_every);
/** @brief Persists the hub's static public key, provisioned out of band. */
int  store_save_hub_static(const uint8_t *pub_c);

/** @brief Reserves a counter block and returns the first value safe to use.
 *  radio_devices_docs/wl55_device/arch/store.md */
int  store_reserve_counter(uint32_t counter_now, uint32_t *first_safe,
                           uint32_t *mark_out);

/** @brief Raises the durable receive floor; amortised to block boundaries. */
int  store_note_received(uint32_t counter);

/** @brief Reports the floor's key generation, leaving the comparison to the caller. */
int  store_key_gen(uint32_t *gen);

/** @brief The durable PAIR_INIT replay ceiling.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int  store_init_ceiling(uint32_t *out);

/** @brief Raises the ceiling; only ever called after a MAC has verified. */
int  store_save_init_ceiling(uint32_t superframe);

/** @brief Counts records of the previous format: unreadable is not absent.
 *  radio_devices_docs/wl55_device/arch/store.md */
int  store_legacy_present(uint32_t *dev_id_out);

/** @brief Persists the network a PAIR_INIT must claim to be from. */
int  store_save_network(uint32_t hub_id, uint16_t net_id);

/** @brief Test hook: writes the short record a torn write leaves behind. */
int  store_write_torn(void);

/** @brief Erases both pages, losing the identity with them. */
int  store_erase_all(void);

/** @brief Occupancy, so the append point can be watched rather than assumed. */
void store_stats(uint32_t *records, uint32_t *slots_free, uint32_t *slots_total);
