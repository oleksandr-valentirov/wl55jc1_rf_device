/* The node's own state machine, started by nothing anyone types.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "build_id.h"
#include "main.h"
#include "device.h"
#include "vcp.h"
#include "clock.h"
#include "radio.h"
#include "crypto.h"
#include "frame.h"
#include "pairing.h"
#include "store.h"
#include "superframe.h"
#include "hop.h"
#include "hop_prf.h"
#include "load.h"
#include "hop_vectors.h"
#include "vectors.h"
#include "join.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "radio_phy.h"
#include "pair_init.h"
#include "beacon.h"
#include "wire_v4.h"
#include "telemetry.h"
#include "sensor.h"
#include "upseq.h"

extern UART_HandleTypeDef hcom_uart[];

/* Console time is charged separately: a cost of the rig, not of the protocol.
 * radio_devices_docs/wl55_device/testing/console.md */


/* Running stats for the two capture instruments, so both report one shape. */
typedef struct {
    uint32_t n, sum, lo, hi, noise;
} gap_stats_t;

/* The preamble detector fires on noise. Nothing past 32 preamble bytes is
 * physical. */


#define BUILD_ID_RECORD  (BUILD_ID_COMMIT | (BUILD_ID_DIRTY ? 0x80000000u : 0u))




/* Sealed here must equal the shared vector byte for byte: proves AAD and nonce assembly.
 * radio_devices_docs/wl55_device/testing/console.md */
static pairing_ctx_t pair_ctx;
/* Set when a downlink grants a rate, cleared once it reaches flash. */
static uint8_t rate_unsaved;

/* An opened downlink proves the clock, the key and the hub's counter at once.
 * radio_devices_docs/radio/decisions/0023-the-hub-supplies-the-transmit-floor.md */
static uint32_t tx_floor;
static uint8_t  tx_floor_known;

/* The free run may aim one superframe past the last beacon, so clear two.
 * radio_devices_docs/radio/decisions/0023-the-hub-supplies-the-transmit-floor.md */
#define TX_FLOOR_MARGIN  2u

static int tx_allowed(uint32_t sf) {
    return tx_floor_known && (int32_t)(sf - tx_floor - TX_FLOOR_MARGIN) > 0;
}

static superframe_t sframe;
static quiesce_t quiesce;

static void time_start(void) {
    if (sframe.g.running)
        return;
    /* Zero: a booted device has no opinion and the first beacon gives it one.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    superframe_start(&sframe, 0u, SUPERFRAME_STUB_US);
}

static int8_t beacon_rssi_dbm;
static uint8_t beacon_rssi_valid;

/** @brief Records an accepted beacon's level, from whichever path heard it. */
static void beacon_rssi_note(int16_t dbm)
{
    beacon_rssi_dbm  = (int8_t)dbm;
    beacon_rssi_valid = 1u;
}

static int join_is_paired(void);
static int hop_channel_live(uint32_t sf, uint8_t *ch);

#define DOWNLINK_RX_LEAD_US  8000u
_Static_assert(RADIO_DOWNLINK_RX_OPEN_US - DOWNLINK_RX_LEAD_US >
                   RADIO_AIR_START_TO_END_US(sizeof(radio_data_beacon_t)),
               "receive lead would open inside the beacon and eat it instead");

/* How long a receive stays open for a beacon that should already be there. */
#define REPORT_BEACON_WINDOW_US   40000u


/* Identity comes off flash: a keypair redrawn per reset is not an identity.
 * radio_devices_docs/wl55_device/testing/console.md */


/* The published PAIR_INIT through the live verify path; the Z1 seed is what allows it.
 * radio_devices_docs/wl55_device/radio/pairing.md */


/* Returns 0 if anything is missing rather than filling a field with a plausible zero.
 * radio_devices_docs/wl55_device/testing/console.md */
static int pair_init_live_ctx(pair_init_ctx_t *c, store_state_t *st) {
    store_init(st);
    /* hub_id 0 is allowed for listening: an unpaired device knows no network.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (!st->valid)
        return 0;
    c->dev_priv   = st->priv;
    c->dev_id     = st->dev_id;
    c->hub_id     = st->hub_id;
    c->net_id     = st->net_id;
    /* This device's own mode, never the frame's claim. ADR-0024 */
    c->enrol_mode = RADIO_ENROL_MODE_OPEN;
    return 1;
}


/* Printed, not checked: one build here, so a digest can only disagree elsewhere.
 * radio_devices_docs/wl55_device/testing/console.md */


static join_result_t join_res;

/* Exactly 2*len digits or nothing: a short parse derives against a prefix.
 * radio_devices_docs/wl55_device/testing/console.md */

/* The generation separates a grant just negotiated from one restored from flash.
 * radio_devices_docs/wl55_device/testing/console.md */


static hop_ctx_t hopper;

static int hop_init_live(void) {
    const uint8_t *key = join_res.paired ? join_res.hop_key : vec_hop_key;

    return hop_init(&hopper, hop_prf_aes, (void *)key, RADIO_HOP_COUNT);
}

static int join_is_paired(void) { return join_res.paired != 0u; }

static int hop_channel_live(uint32_t sf, uint8_t *ch) {
    return (hop_init_live() != 0) ? -1 : hop_channel(&hopper, sf, ch);
}

/* Reports which convention the other end used rather than leaving it guessed.
 * radio_devices_docs/wl55_device/testing/console.md */

/* Idle is what is left over, so it is a lower bound on load and not an upper one.
 * radio_devices_docs/wl55_device/testing/console.md */

#include "link_v7.h"   /* both data frames at 39 bytes, and the only downlink vector */

/* A size assert answers "same shape", never "same contract": v4 built clean on v5.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
_Static_assert(LINK_VECTORS_VERSION == RADIO_LINK_VERSION,
               "the link vectors are not the wire this build speaks");

/* The grid is in hub microseconds; this clock is fast and lands early.
 * radio_devices_docs/wl55_device/radio/timebase.md */
static uint32_t hub_us_to_local(uint32_t hub_us);

/* What the next report echoes. The wire cannot tell its zero from cmd 0 seq 0. */
static uint8_t  dl_ack_seq, dl_ack_cmd, dl_ack_arg, dl_any;
static uint32_t dl_applied, dl_repeats, dl_replays, dl_opened;

/* Held and never interpreted; the witness is what was acknowledged for them.
 * radio_devices_docs/radio/decisions/0037-the-application-payload-is-the-downlinks-and-the-frame-does-not-grow.md */
static uint8_t dl_app[6], dl_app_len, dl_app_any, dl_app_witness;
static uint32_t dl_app_bad;

/* What the hub measured on this node's uplink, which nothing here can measure.
 * radio_devices_docs/radio/decisions/0037-the-application-payload-is-the-downlinks-and-the-frame-does-not-grow.md */
static int8_t  hub_rssi_up_dbm;
static uint8_t hub_rssi_up_valid;

/* Attempts, not deliveries: the hub's frames_ok is the numerator over this. */
static upseq_t up_seq;

/* A silence this device imposed on itself. A reboot is not one: uptime_s says that.
 * radio_devices_docs/radio/tdma.md */
static uint8_t tx_self_silenced;
static uint8_t tx_hold_said;

/* A counter, not the hub's tuple: this slot field never varies.
 * radio_devices_docs/wl55_device/security/replay.md */
static uint32_t dl_floor;
static uint8_t  dl_floor_known;
/* Kept so the floor's control re-feeds a frame that really was accepted. */
static uint8_t  last_dl[sizeof(radio_downlink_t)];
static uint8_t  have_last_dl;

static int downlink_open_with(const uint8_t *f, uint8_t len, uint8_t my_slot,
                              const uint8_t *key, uint32_t dev,
                              radio_downlink_cmd_t *cmd);

/* One construction both ways; only the direction byte keeps the two nonce spaces apart.
 * radio_devices_docs/wl55_device/testing/console.md */
static void frame_nonce(uint32_t sf, uint32_t dev, uint8_t dir, uint8_t slot,
                        uint8_t *nonce) {
    nonce[0] = (uint8_t)(sf >> 24);  nonce[1] = (uint8_t)(sf >> 16);
    nonce[2] = (uint8_t)(sf >> 8);   nonce[3] = (uint8_t)sf;
    nonce[4] = (uint8_t)(dev >> 24); nonce[5] = (uint8_t)(dev >> 16);
    nonce[6] = (uint8_t)(dev >> 8);  nonce[7] = (uint8_t)dev;
    nonce[8] = dir;
    nonce[9] = 0; nonce[10] = 0; nonce[11] = slot;
}

/* Split out so it can be checked against a published frame, not against my own open.
 * radio_devices_docs/wl55_device/testing/console.md */
static int uplink_build(uint32_t sf, uint8_t slot, uint32_t dev,
                        const uint8_t *key, const radio_uplink_report_t *rep,
                        uint8_t *f) {
    uint8_t nonce[12];

    f[0] = RADIO_FRAME_UPLINK;
    f[1] = RADIO_LINK_VERSION;
    f[2] = slot;
    f[3] = (uint8_t)sf;             /* wire fields little-endian */
    f[4] = (uint8_t)(sf >> 8);
    f[5] = (uint8_t)(sf >> 16);
    f[6] = (uint8_t)(sf >> 24);

    /* Everything the crypto sees is big-endian, including this. */
    frame_nonce(sf, dev, RADIO_DIR_UPLINK, slot, nonce);

    return crypto_gcm_seal(key, nonce, f, 7u, (const uint8_t *)rep,
                           (uint16_t)sizeof(*rep), f + 7, f + 7 + sizeof(*rep));
}

/* One owner of the nonces an uplink has been sealed under.
 * radio_devices_docs/radio/crypto/wire-crypto.md */
static uint32_t seal_last_sf;
static uint8_t  seal_last_slot;
static uint8_t  seal_any;
static uint32_t seal_refused;

/* The pair may not repeat. Mirrors CM4/Core/Src/radio.c in the hub tree. */
static int seal_claim(uint32_t sf, uint8_t slot) {
    /* Signed, so a counter corrected backwards refuses rather than wraps. */
    int32_t age = (int32_t)(sf - seal_last_sf);

    if (seal_any && (age < 0 || (age == 0 && slot <= seal_last_slot))) {
        seal_refused++;
        return 0;
    }
    seal_last_sf = sf;
    seal_last_slot = slot;
    seal_any = 1;
    return 1;
}

/* Clamped to the period tolerance, so the ratio cannot run away; zero is no
 * second beacon yet. */
static uint32_t hub_us_to_local(uint32_t hub_us) {
    if (sframe.measured_us == 0u)
        return hub_us;
    return (uint32_t)(((uint64_t)hub_us * sframe.measured_us) / SUPERFRAME_US);
}

/* The only door to uplink_build for anything that transmits. */
static int uplink_seal(uint32_t sf, uint8_t slot, uint32_t dev,
                       const uint8_t *key, const radio_uplink_report_t *rep,
                       uint8_t *f) {
    if (!seal_claim(sf, slot))
        return -2;
    return uplink_build(sf, slot, dev, key, rep, f);
}

/* Lead, not guard: SetTx to first bit is 24686 us. Aimed mid-slack, all of which is late.
 * radio_devices_docs/wl55_device/testing/console.md */
#define UPLINK_AIM_US   700u    /* half the guard */
#define UPLINK_LEAD_US  (2882u - UPLINK_AIM_US + 268u)  /* ramp - aim + SetTx lag */

/* The device's own slot once: slot offset, counter's channel, and the ramp as lead time.
 * radio_devices_docs/wl55_device/testing/console.md */


/* The uplink's assembly with one byte changed, which is the byte that matters.
 * radio_devices_docs/wl55_device/testing/console.md */
static int downlink_open_with(const uint8_t *f, uint8_t len, uint8_t my_slot,
                              const uint8_t *key, uint32_t dev,
                              radio_downlink_cmd_t *cmd) {
    uint8_t nonce[12];
    uint32_t sf;

    if (len != sizeof(radio_downlink_t) || f[0] != RADIO_FRAME_DOWNLINK ||
        f[1] != RADIO_LINK_VERSION)
        return -1;
    if (f[2] != my_slot)
        return -2;                      /* addressed to another device */
    sf = (uint32_t)f[3] | ((uint32_t)f[4] << 8) |
         ((uint32_t)f[5] << 16) | ((uint32_t)f[6] << 24);

    frame_nonce(sf, dev, RADIO_DIR_DOWNLINK, f[2], nonce);

    if (crypto_gcm_open(key, nonce, f, RADIO_DOWNLINK_AAD_LEN,
                        f + RADIO_DOWNLINK_AAD_LEN, (uint16_t)sizeof(*cmd),
                        (uint8_t *)cmd,
                        f + RADIO_DOWNLINK_AAD_LEN + sizeof(*cmd)) != 0)
        return -3;                      /* tag - wrong key, slot or superframe */
    return 0;
}

/* The floor lives here and not in the caller: two call sites, one of them could forget.
 * radio_devices_docs/wl55_device/security/replay.md */
static int downlink_open(const uint8_t *f, uint8_t len, radio_downlink_cmd_t *cmd) {
    uint32_t sf;
    int rc = downlink_open_with(f, len, join_res.slot, join_res.session,
                                join_res.dev_id_be, cmd);
    if (rc != 0)
        return rc;
    sf = (uint32_t)f[3] | ((uint32_t)f[4] << 8) |
         ((uint32_t)f[5] << 16) | ((uint32_t)f[6] << 24);
    /* Only after the tag: refusing first moves this end's window on a forgery. */
    if (dl_floor_known && (int32_t)(sf - dl_floor) <= 0) {
        tlm_emit(TLM_RX_CMD, sf, cmd->cmd, cmd->cmd_seq, 3u);
        memset(cmd, 0, sizeof(*cmd));
        dl_replays++;
        return -4;
    }
    dl_floor = sf;
    dl_floor_known = 1;
    dl_opened++;
    /* Latched at the first: a later one re-raises a floor already cleared.
     * radio_devices_docs/radio/decisions/0023-the-hub-supplies-the-transmit-floor.md */
    if (!tx_floor_known) {
        tx_floor = sf;
        tx_floor_known = 1;
    }
    memcpy(last_dl, f, sizeof(last_dl));
    have_last_dl = 1;
    return 0;
}

/* Read at boot, because a reset that forgets the floor reopens the window it closed.
 * radio_devices_docs/wl55_device/security/replay.md */
static void dl_floor_load(void) {
    store_state_t st;
    if (store_init(&st) != 0 || !st.valid || st.rx_floor == 0u)
        return;
    dl_floor = st.rx_floor;
    dl_floor_known = 1;
}

/* An unknown cmd is acked, not refused: the ack says only that it arrived. */
static void downlink_apply(uint32_t sf, const radio_downlink_cmd_t *cmd) {
    /* It rides the keepalive and carries no seq, so it is read before the gate.
     * radio_devices_docs/radio/decisions/0037-the-application-payload-is-the-downlinks-and-the-frame-does-not-grow.md */
    if (cmd->cmd == RADIO_CMD_LINK) {
        hub_rssi_up_dbm   = RADIO_LINK_ARG_TO_DBM(cmd->arg);
        hub_rssi_up_valid = 1u;
        tlm_emit(TLM_RX_LINK, sf, cmd->arg & 0xFFu,
                 (uint32_t)(int32_t)hub_rssi_up_dbm, 0u);
    }
    /* The keepalive names no command, so it must not clear the echo of one. */
    if (cmd->cmd_seq == RADIO_CMD_SEQ_NONE) {
        tlm_emit(TLM_RX_CMD, sf, cmd->cmd, cmd->cmd_seq, 2u);
        return;
    }
    if (dl_any && cmd->cmd_seq == dl_ack_seq && cmd->cmd == dl_ack_cmd) {
        dl_repeats++;
        tlm_emit(TLM_RX_CMD, sf, cmd->cmd, cmd->cmd_seq, 1u);
        return;
    }
    /* A length past its array is unacked rather than clamped. */
    if (cmd->cmd == RADIO_CMD_APP && cmd->app_len > sizeof(cmd->app)) {
        dl_app_bad++;
        tlm_emit(TLM_RX_CMD, sf, cmd->cmd, cmd->cmd_seq, 4u);
        return;
    }
    if (cmd->cmd == RADIO_CMD_SET_RATE && cmd->report_every != 0u) {
        join_res.report_every = cmd->report_every;
        rate_unsaved = 1u;
    }
    if (cmd->cmd == RADIO_CMD_APP) {
        /* Cleared first: a shorter command must not leave the tail of a longer one. */
        memset(dl_app, 0, sizeof(dl_app));
        memcpy(dl_app, cmd->app, cmd->app_len);
        dl_app_len     = cmd->app_len;
        dl_app_any     = 1u;
        dl_app_witness = radio_app_witness(cmd->app, cmd->app_len);
        tlm_emit(TLM_RX_APP, sf, cmd->app_len,
                 (uint32_t)dl_app[0] | ((uint32_t)dl_app[1] << 8) |
                 ((uint32_t)dl_app[2] << 16) | ((uint32_t)dl_app[3] << 24),
                 (uint32_t)dl_app[4] | ((uint32_t)dl_app[5] << 8));
    }
    dl_ack_seq = cmd->cmd_seq;
    dl_ack_cmd = cmd->cmd;
    /* What was applied, not what was asked; APP answers with its own witness. */
    if (cmd->cmd == RADIO_CMD_SET_RATE)
        dl_ack_arg = join_res.report_every;
    else if (cmd->cmd == RADIO_CMD_APP)
        dl_ack_arg = dl_app_witness;
    else
        dl_ack_arg = 0u;
    dl_any     = 1u;
    dl_applied++;
    tlm_emit(TLM_RX_CMD, sf, cmd->cmd, cmd->cmd_seq, 0u);
}

/* The array the view copies out must be the array the wire brings. ADR-0037 */
_Static_assert(sizeof(dl_app) == sizeof(((radio_downlink_cmd_t *)0)->app),
               "the held application bytes are not the downlink's");


/* Reports bytes and refuses to interpret them: the window and rate are contract.
 * radio_devices_docs/wl55_device/testing/console.md */

/* The granted cadence, disarmed after a reset.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#define REPORT_BOUNDARY_LEAD_US  100000u
/* Caps the blocking receive; past that only a re-camp helps. */
#define REPORT_UNCERT_MAX_US     120000u

/* Which of the k opportunities the loop uses. radio_devices_docs/radio/tdma.md */
static uint8_t  report_opp;
/* Use every k rather than one, for repeats on a single channel. */
static uint8_t  report_opp_all;
static int16_t  arm_dbm = 0x7FFF;   /* no setting yet announced */
static uint8_t  arm_opp, arm_every;
/* 0 any, 1 below the join channel, 2 above: makes a counting receiver
 * discriminate. radio_devices_docs/radio/hopping.md */
static uint8_t  report_band;
static uint32_t report_attempt_sf;
static uint32_t reports_sent, beacons_missed;
/* The populations both receive counters are fractions of.
 * radio_devices_docs/wl55_device/radio/beacon.md */
static uint32_t beacon_windows, dl_windows;
static uint32_t invite_slices, invites_refused;

/* Coming back after the counter is gone.
 * radio_devices_docs/radio/joining.md */
#define RECOVER_LOST_US        30000000u
/* One pass holds the superloop for this long and no longer. */
#define RECOVER_CHUNK_US         200000u
#define RECOVER_UNCERT_MAX_US    250000u
#define RECOVER_SEARCH_WINDOW_US  60000u
/* Predicted windows tried before the counter is treated as gone. */
#define RECOVER_SEARCH_TRIES          4u

typedef enum {
    RECOVER_IDLE = 0,   /* a beacon arrived recently enough to schedule on */
    RECOVER_SEARCH,     /* a measured period survives, so predict and open a window */
    RECOVER_PARK        /* no usable counter: hold one channel until the hop lands */
} recover_state_t;

static recover_state_t recover_state;
static uint8_t  recover_park_grid, recover_park_forced;
static uint32_t recover_park_sf;    /* superframe the parked channel accepted */
static uint32_t recover_entered, recover_search_windows, recover_search_hits;
static uint32_t recover_search_tries;
static uint32_t recover_park_chunks, recover_park_hits;
/* The parked device's only evidence, so counted apart from other refusals. */
static uint32_t recover_mismatch, recover_rejected;
static uint32_t recover_mismatch_sf;
static uint8_t  recover_mismatch_grid;
static beacon_rc_t recover_last_rc;
/* Read over SWD: a wedge takes the console, so a stopped core reads as one.
 * radio_devices_docs/wl55_device/radio/timebase.md */
volatile uint32_t loop_mark;
volatile uint32_t loop_beat;


/* The one check that survives having no clock.
 * radio_devices_docs/radio/joining.md */
static int recover_channel_agrees(uint32_t claimed, uint8_t grid) {
    uint8_t ch;

    if (hop_channel_live(claimed, &ch) != 0)
        return 0;
    return hop_to_grid(ch) == grid;
}

/* Non-zero when the clock was aligned. */
static int recover_take(const uint8_t *rx, const radio_rx_info_t *info, uint8_t grid) {
    uint32_t claimed = 0, aligned = 0;
    uint32_t before = superframe_now(&sframe);
    beacon_rc_t rc = beacon_peek(rx, info->len, &claimed);

    if (rc != BEACON_OK) {
        recover_last_rc = rc;
        recover_rejected++;
        return 0;
    }
    if (!recover_channel_agrees(claimed, grid)) {
        recover_mismatch++;
        recover_mismatch_sf = claimed;
        recover_mismatch_grid = grid;
        tlm_emit(TLM_REC_DENY, claimed, grid, TLM_WHY_CHANNEL, 0u);
        return 0;
    }
    rc = beacon_apply(rx, info->len, &sframe, &quiesce, info->start_us, &aligned);
    if (rc != BEACON_OK) {
        recover_last_rc = rc;
        recover_rejected++;
        tlm_emit(TLM_REC_DENY, claimed, grid, TLM_WHY_BEACON, 0u);
        return 0;
    }
    recover_last_rc = BEACON_OK;
    recover_park_sf = aligned;
    beacon_rssi_note(info->rssi_dbm);
    tlm_emit(TLM_REC_HIT, aligned, grid, (uint32_t)(int32_t)info->rssi_dbm, 0u);
    if (aligned != before)
        tlm_emit(TLM_SYNC_JUMP, aligned, before, (uint32_t)(int32_t)(aligned - before), 0u);
    /* Predicting the next beacon beats holding a channel another cycle. */
    if (recover_state == RECOVER_PARK && sframe.g.running && sframe.aligned) {
        recover_state = RECOVER_SEARCH;
        recover_search_tries = 0;
    }
    return 1;
}

/* Where the counter says the beacon is, widened by the free run's drift. */
static void recover_search(void) {
    uint8_t rx[32], hop, grid;
    radio_rx_info_t info = {0};

    uint32_t sf = superframe_now(&sframe) + 1u;
    uint32_t stale = micros() - sframe.last_beacon_us;
    uint32_t uncert = stale / 1000u;

    if (uncert > RECOVER_UNCERT_MAX_US)
        uncert = RECOVER_UNCERT_MAX_US;

    uint32_t boundary = (sframe.g.start + sframe.g.period);
    if ((int32_t)(boundary - micros()) > (int32_t)(REPORT_BOUNDARY_LEAD_US + uncert))
        return;
    if (hop_channel_live(sf, &hop) != 0 || radio_configure((grid = hop_to_grid(hop))) != 0)
        return;

    recover_search_windows++;
    recover_search_tries++;
    while (!timebase_elapsed(boundary - DOWNLINK_RX_LEAD_US - uncert)) { }
    info.timeout_us = RECOVER_SEARCH_WINDOW_US + 2u * uncert;
    if (radio_receive(rx, sizeof(rx), &info) == 0 && recover_take(rx, &info, grid)) {
        recover_search_hits++;
        recover_search_tries = 0;
        return;
    }
    if (recover_search_tries >= RECOVER_SEARCH_TRIES) {
        recover_state = RECOVER_PARK;
        recover_park_grid = 0xFFu;
    }
}

/* One channel held: the hop visits each once a cycle, so the wait is bounded.
 * radio_devices_docs/radio/joining.md */
static void recover_park(void) {
    uint8_t rx[32];
    radio_rx_info_t info = {0};

    if (recover_park_grid == 0xFFu) {
        uint32_t r = 0;
        /* A fixed choice would put every lost device on one known channel. */
        if (radio_rng_word(&r) != 0)
            r = join_res.dev_id_be;
        recover_park_grid = hop_to_grid((uint8_t)(r % RADIO_HOP_COUNT));
        tlm_emit(TLM_REC_PARK, sframe.g.counter, recover_park_grid,
                 radio_slot_hz(recover_park_grid), 0u);
    }
    /* Re-tuned every pass: a park drifted off its channel waits forever. */
    if (radio_configure(recover_park_grid) != 0)
        return;

    recover_park_chunks++;
    info.timeout_us = RECOVER_CHUNK_US;
    if (radio_receive(rx, sizeof(rx), &info) != 0)
        return;
    if (recover_take(rx, &info, recover_park_grid))
        recover_park_hits++;
}

void recover_service(void) {
    if (!join_is_paired())
        return;
    /* Nothing typed starts the clock on this path. */
    time_start();

    /* One beacon aligns and leaves the stub period: aligned and unusable. */
    if (!recover_park_forced && superframe_can_schedule(&sframe)) {
        if (recover_state != RECOVER_IDLE)
            tlm_emit(TLM_REC_EXIT, sframe.g.counter, sframe.measured_us, 0u, 0u);
        recover_state = RECOVER_IDLE;
        recover_search_tries = 0;
        return;
    }
    if (recover_state == RECOVER_IDLE) {
        if (!recover_park_forced &&
            superframe_since_beacon_us(&sframe) < RECOVER_LOST_US)
            return;
        recover_entered++;
        recover_search_tries = 0;
        tlm_emit(TLM_REC_ENTER, sframe.g.counter,
                 (sframe.measured_us != 0u && sframe.g.running) ? 1u : 2u, 0u, 0u);
        /* No measured period is nothing to extrapolate from. */
        if (sframe.measured_us != 0u && sframe.g.running) {
            recover_state = RECOVER_SEARCH;
        } else {
            recover_state = RECOVER_PARK;
            recover_park_grid = 0xFFu;
        }
    }
    if (recover_state == RECOVER_SEARCH)
        recover_search();
    else
        recover_park();

    /* The control exists to watch the accept, so it ends there. */
    if (recover_park_forced && superframe_can_schedule(&sframe)) {
        recover_park_forced = 0;
        recover_state = RECOVER_IDLE;
    }
}


void report_service(void) {
    uint8_t rx[32], f[sizeof(radio_uplink_t)], hop, grid;
    radio_rx_info_t info = {0};
    radio_uplink_report_t rep;
    sensor_reading_t meas;
    uint32_t aligned = 0;

    if (!join_is_paired() || sframe.measured_us == 0u)
        return;
    /* Two services retuning leaves neither one's window open. */
    if (recover_state != RECOVER_IDLE)
        return;

    uint32_t sf = superframe_now(&sframe) + 1u;
    /* Anchored to the hub's counter: counting from the last send re-phases.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    if (join_res.report_every == 0u)
        return;
    /* Floorless has nothing to send, so it opens every downlink opportunity.
     * radio_devices_docs/wl55_device/radio/beacon.md */
    uint8_t listen = (uint8_t)(!tx_floor_known && RADIO_DOWNLINK_ON(sf));
    if ((sf % join_res.report_every) != 0u && !listen)
        return;
    if (report_attempt_sf == sf)
        return;

    /* Sized by staleness: a fixed window makes each miss likelier. */
    uint32_t stale = micros() - sframe.last_beacon_us;
    uint32_t uncert = stale / 1000u;               /* 1000 ppm of the free run */
    if (uncert > REPORT_UNCERT_MAX_US)
        uncert = REPORT_UNCERT_MAX_US;

    /* Only near a boundary, so the superloop is not blocked waiting for one. */
    uint32_t boundary = (sframe.g.start + sframe.g.period);
    if ((int32_t)(boundary - micros()) > (int32_t)(REPORT_BOUNDARY_LEAD_US + uncert))
        return;

    report_attempt_sf = sf;
    if (hop_channel_live(sf, &hop) != 0 || radio_configure((grid = hop_to_grid(hop))) != 0)
        return;

    /* Re-align first; the beacon says the free run was good enough. */
    while (!timebase_elapsed(boundary - DOWNLINK_RX_LEAD_US - uncert)) { }
    info.timeout_us = REPORT_BEACON_WINDOW_US + 2u * uncert;
    beacon_windows++;
    if (radio_receive(rx, sizeof(rx), &info) != 0 ||
        beacon_apply(rx, info.len, &sframe, &quiesce, info.start_us, &aligned) != BEACON_OK) {
        /* A counted miss with no record is a rate nobody can date. */
        beacons_missed++;
        tlm_emit(TLM_RX_MISS, sf, info.timeout_us, grid, 0u);
        return;
    }
    beacon_rssi_note(info.rssi_dbm);
    /* One per cycle: denominator, window, and whether the stamp was captured. */
    tlm_emit(TLM_RX_BEACON, sf, info.timeout_us,
             (info.capture_valid && info.capture_sync) ? 0u : 1u, 0u);
    if (quiesce_active(&quiesce, aligned))
        return;

    /* The anchor is the hub's counter, not this node's guess at it. */
    sf = aligned;
    /* The radio is tuned for report_attempt_sf; a different counter is a
     * different channel. radio_devices_docs/radio/hopping.md */
    if (sf != report_attempt_sf) {
        tlm_emit(TLM_TX_DENY, sf, TLM_WHY_CHANNEL, grid, report_attempt_sf);
        return;
    }
    /* Read before the downlink block: a rate granted there is next cycle's. */
    uint8_t granted = (uint8_t)((sf % join_res.report_every) == 0u);
    /* After the beacon: skipping the receive would confound this with
     * staleness. */
    if ((report_band == 1u && grid > radio_join_slot()) ||
        (report_band == 2u && grid < radio_join_slot())) {
        tlm_emit(TLM_TX_DENY, sf, TLM_WHY_BAND, grid, 0u);
        return;
    }
    /* The region is between the beacon and slot 1, so a command is echoed in
     * the same cycle. radio_devices_docs/radio/tdma.md */
    if (RADIO_DOWNLINK_ON(sf)) {
        uint8_t dl[sizeof(radio_downlink_t)];
        radio_rx_info_t di = {0};
        radio_downlink_cmd_t cmd;
        uint32_t open_at = sframe.last_beacon_us
                           + hub_us_to_local(RADIO_DOWNLINK_RX_OPEN_US)
                           - DOWNLINK_RX_LEAD_US;

        /* A region already past is skipped: waiting for it costs the k=0 slot. */
        if (!timebase_elapsed(open_at)) {
            while (!timebase_elapsed(open_at)) { }
            di.timeout_us = RADIO_DOWNLINK_RX_CLOSE_US - RADIO_DOWNLINK_RX_OPEN_US
                            + DOWNLINK_RX_LEAD_US;
            dl_windows++;
            /* An empty window and a refused frame were one silence.
             * radio_devices_docs/wl55_device/radio/beacon.md */
            if (radio_receive(dl, sizeof(dl), &di) != 0) {
                tlm_emit(TLM_RX_DL_MISS, sf, di.timeout_us, grid, 0u);
            } else {
                int rc = downlink_open(dl, di.len, &cmd);

                if (rc == 0)
                    downlink_apply(sf, &cmd);
                else
                    tlm_emit(TLM_RX_DL_MISS, sf, di.timeout_us, grid,
                             (uint32_t)(-rc));
            }
        }
    }

    /* The extra windows are receive only: the grant alone says what may transmit.
     * radio_devices_docs/wl55_device/radio/beacon.md */
    if (!granted) {
        tlm_emit(TLM_TX_DENY, sf, TLM_WHY_OFFBEAT, 0u, 0u);
        return;
    }

    memset(&rep, 0, sizeof(rep));
    rep.rssi_down = beacon_rssi_valid ? beacon_rssi_dbm : 0;
    if (!beacon_rssi_valid)
        rep.flags |= RADIO_REPORT_FLAG_RSSI_STALE;
    /* One conversion pair answers for both, so one refusal marks both stale.
     * radio_devices_docs/wl55_device/arch/sensors.md */
    if (sensor_read(&meas) == 0) {
        rep.supply_mv  = meas.supply_mv;
        rep.temp_c_x10 = meas.temp_c_x10;
    } else {
        rep.flags |= RADIO_REPORT_FLAG_SUPPLY_STALE | RADIO_REPORT_FLAG_TEMP_STALE;
    }
    rep.uptime_s  = timebase_uptime_s();
    rep.ack_seq   = dl_ack_seq;
    rep.ack_cmd   = dl_ack_cmd;
    rep.ack_arg   = dl_ack_arg;

    /* Unreserved is nonce reuse after a reboot. A refusal that skips the
     * top-up denies for ever. */
    uint8_t may_send = (uint8_t)(tx_allowed(sf) != 0);
    /* Once, and it names which of the two gates is shut. ADR-0023 */
    if (!may_send && !tx_hold_said) {
        tx_hold_said = 1;
        tlm_emit(TLM_TX_HOLD, sf,
                 tx_floor_known ? TLM_WHY_FLOOR : TLM_WHY_NODOWNLINK,
                 tx_floor_known ? tx_floor : 0u, 0u);
    }
    if (may_send)
        tx_hold_said = 0;
    if (!may_send) {
        /* The two gates tx.hold already tells apart; one name for both hid a node. */
        tlm_emit(TLM_TX_DENY, sf,
                 tx_floor_known ? TLM_WHY_FLOOR : TLM_WHY_NODOWNLINK, 0u, 0u);
        tx_self_silenced = 1;
    }
    if (tx_self_silenced)
        rep.flags |= RADIO_REPORT_FLAG_RESUMED;

    /* Every k is the same channel a second apart: a receiver's own spread. */
    uint8_t k_first = report_opp_all ? 0u : report_opp;
    uint8_t k_last  = report_opp_all ? (uint8_t)(RADIO_SLOT_OPPS - 1u) : report_opp;

    /* A setting changed in place announces nothing otherwise.
     * radio_devices_docs/radio/known-issues.md */
    int16_t dbm = (int16_t)radio_power_dbm();
    uint8_t opp = report_opp_all ? 0xFFu : report_opp;
    if (dbm != arm_dbm || opp != arm_opp || join_res.report_every != arm_every) {
        arm_dbm   = dbm;
        arm_opp   = opp;
        arm_every = join_res.report_every;
        tlm_emit(TLM_TX_ARM, sf, (uint32_t)(int32_t)dbm, opp, arm_every);
    }

    for (uint8_t k = k_first; may_send && k <= k_last; k++) {
        /* The slot number is sealed into the nonce, so it is the real one and
         * not the device index. radio_devices_docs/radio/crypto/wire-crypto.md */
        uint8_t  slot_n  = (uint8_t)(join_res.slot + k * RADIO_SLOT_STRIDE);
        uint32_t slot_at = sframe.last_beacon_us +
                           hub_us_to_local(RADIO_SLOT_NTH_US(join_res.slot, k));

        /* A late slot is another device's; the receive can overrun the first k. */
        if (timebase_elapsed(slot_at - UPLINK_LEAD_US)) {
            tlm_emit(TLM_TX_DENY, sf, TLM_WHY_LATE, grid, slot_n);
            continue;
        }
        /* Spent at the seal, where the nonce is: a refused seal left no gap. */
        rep.up_seq = upseq_pending(&up_seq);
        if (uplink_seal(sf, slot_n, join_res.dev_id_be, join_res.session,
                        &rep, f) != 0) {
            tlm_emit(TLM_TX_DENY, sf, TLM_WHY_SEAL, 0u, 0u);
            continue;
        }
        upseq_commit(&up_seq);

        uint32_t air = 0;
        while (!timebase_elapsed(slot_at - UPLINK_LEAD_US)) { }
        if (radio_send(f, sizeof(f), &air) != 0) {
            tlm_emit(TLM_TX_DENY, sf, TLM_WHY_RADIO, 0u, 0u);
            continue;
        }
        /* The same arithmetic cmd_uplink prints, for one population. */
        uint32_t on_air = radio_tx_air_time_us((uint8_t)sizeof(f));
        uint32_t ramp = (air > on_air) ? (air - on_air) : 0u;
        int32_t  off = (int32_t)(micros() - air - slot_at) + (int32_t)ramp;

        tlm_emit(TLM_TX_UP, sf, slot_n, (uint32_t)off, grid);
        reports_sent++;
        /* Spent on the frame that carried it, not on the one that was built. */
        tx_self_silenced = 0;
    }

    /* Same gap, same reason: a flash append inside a slot would miss the slot. */
    if (dl_floor_known)
        (void)store_note_received(dl_floor);
    /* The flag is the retry: a refused write stays owed to the next cycle. */
    if (rate_unsaved && store_save_report_every(join_res.report_every) == 0)
        rate_unsaved = 0u;
}

/* Beats bound the micros() wrap at 71.6 minutes.
 * radio_devices_docs/wl55_device/testing/telemetry.md */
#define TLM_BEAT_US  5000000u

static uint32_t tlm_beat_at;
static uint8_t  tlm_was_schedulable;

/* One record a pass, under 8.3 ms, absorbed by the boundary lead. */
void telemetry_service(void) {
    char line[TLM_LINE_MAX];

    if (!tlm_enabled())
        return;

    uint8_t now_ok = (uint8_t)(superframe_can_schedule(&sframe) ? 1u : 0u);
    if (now_ok != tlm_was_schedulable) {
        if (now_ok)
            tlm_emit(TLM_SYNC_OK, superframe_now(&sframe), sframe.measured_us, 0u, 0u);
        else
            tlm_emit(TLM_SYNC_LOST, superframe_now(&sframe),
                     superframe_since_beacon_us(&sframe), 0u, 0u);
        tlm_was_schedulable = now_ok;
    }
    if (timebase_elapsed(tlm_beat_at)) {
        /* Stepped, not measured from now: the drain's cost would drift. */
        tlm_beat_at += TLM_BEAT_US;
        if ((int32_t)(micros() - tlm_beat_at) > 0)
            tlm_beat_at = micros() + TLM_BEAT_US;
        /* Advanced, not read: the counter freezes between recoveries. */
        tlm_emit(TLM_BEAT, timebase_uptime_s(), superframe_now(&sframe),
                 (uint32_t)sframe.state |
                 (superframe_can_schedule(&sframe) ? 0x10u : 0u) |
                 (recover_state != RECOVER_IDLE ? 0x20u : 0u), 0u);
    }

    int n = tlm_next(line, (int)sizeof(line));
    if (n > 0)
        vcp_write((const uint8_t *)line, (uint16_t)n, 100);
}


/* One invitation cadence plus margin: a slice cannot fall between two.
 * radio_devices_docs/radio/joining.md */
#define INVITE_SLICE_MS  ((RADIO_PAIR_INIT_EVERY * (SUPERFRAME_US / 1000u)) + 500u)

/** @brief Draws an identity and stores it, the first time the node ever boots. */
static void provision_identity(void) {
    store_state_t st;
    uint8_t priv[X25519_PRIV_LEN], pub[X25519_PUB_LEN];
    uint32_t id = 0;

    if (store_init(&st) == 0 && st.valid) {
        memcpy(pair_ctx.priv, st.priv, sizeof(pair_ctx.priv));
        /* The point is not stored and must be recomputed, or it stays zero.
         * radio_devices_docs/wl55_device/arch/store.md */
        if (crypto_x25519_public_from_private(pair_ctx.priv, pair_ctx.pub) != 0) {
            tlm_emit(TLM_IDENT, st.dev_id, st.key_gen, 0u, 4u);
            return;
        }
        pair_ctx.dev_id   = st.dev_id;
        pair_ctx.have_key = 1;
        tlm_emit(TLM_IDENT, st.dev_id, st.key_gen, 1u, 0u);
        return;
    }

    /* Drawn, never sequential: a small id is long zero runs in a cleartext header.
     * radio_devices_docs/wl55_device/testing/console.md */
    for (int i = 0; i < 32; i++) {
        if (radio_rng_word(&id) != 0) {
            tlm_emit(TLM_IDENT, 0u, 0u, 0u, 1u);
            return;
        }
        uint8_t b0 = (uint8_t)id, b1 = (uint8_t)(id >> 8);
        uint8_t b2 = (uint8_t)(id >> 16), b3 = (uint8_t)(id >> 24);
        if (b0 != b1 && b1 != b2 && b2 != b3 && b0 != b2 && b0 != b3 && b1 != b3)
            break;
    }
    if (crypto_x25519_keygen(priv, pub) != 0) {
        tlm_emit(TLM_IDENT, 0u, 0u, 0u, 2u);
        return;
    }
    if (store_save_identity(priv, id) != 0) {
        tlm_emit(TLM_IDENT, 0u, 0u, 0u, 3u);
        return;
    }
    memcpy(pair_ctx.priv, priv, sizeof(priv));
    memcpy(pair_ctx.pub, pub, sizeof(pub));
    pair_ctx.have_key = 1;
    pair_ctx.dev_id = id;
    tlm_emit(TLM_IDENT, id, 0u, 0u, 0u);
}

/* An unpairable device idles rather than spinning; the old path burned 139/s. */
#define INVITE_RETRY_GAP_MS     2000u

static uint32_t invite_last_ms;
/* Boot is only the first physical act; a release is another.
 * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
static uint32_t enrol_window_from_s;
/* The close is announced once; a reopen has to be able to announce it again. */
static uint8_t  enrol_shut_said;

/** @brief Listens one slice for an invitation; runs only while unpaired. */
static void invite_service(void) {
    pair_init_ctx_t c = {0};
    store_state_t st;
    uint32_t now = millis_hw();

    if ((timebase_uptime_s() - enrol_window_from_s) * 1000u > DEVICE_ENROL_WINDOW_MS) {
        /* Said once, so the console's "listening" and the record agree. ADR-0024 */
        if (!enrol_shut_said) {
            enrol_shut_said = 1;
            tlm_emit(TLM_JOIN_TRY, invite_slices, 0u, 0u, 2u);
        }
        return;
    }
    if (invite_last_ms != 0u && (uint32_t)(now - invite_last_ms) < INVITE_RETRY_GAP_MS)
        return;
    invite_last_ms = now;

    /* Nothing to answer an invitation with: the identity is the node's half. */
    if (!pair_init_live_ctx(&c, &st))
        return;

    invite_slices++;
    int rc = join_run_invited(&pair_ctx, &join_res, INVITE_SLICE_MS, &c,
                              st.init_ceiling);
    if (rc == 0) {
        tlm_emit(TLM_JOIN_OK, join_res.slot, join_res.report_every,
                 join_res.superframe, 0u);
        return;
    }
    if (rc == -30) {
        join_stats_t s2;
        join_stats(&s2);
        invites_refused++;
        tlm_emit(TLM_JOIN_TRY, invite_slices, (uint32_t)(-rc),
                 s2.invite_refused, 1u);
        return;
    }
    tlm_emit(TLM_JOIN_TRY, invite_slices, (uint32_t)(-rc), 0u, 0u);
}

#if WL55_DEV_COMMANDS
/* The k=3 geometry as an instrument, which is all it has ever been. ROADMAP 77 */
int device_set_opportunities(uint8_t k, uint8_t all) {
    if (!all && k >= RADIO_SLOT_OPPS)
        return -1;
    report_opp_all = all ? 1u : 0u;
    report_opp     = all ? 0u : k;
    /* Re-announce: arm_opp gates the record, so a silent change loses the boundary. */
    arm_opp = 0xFEu;
    return 0;
}
#endif

/* Keeps the identity and drops the grant, which are one store today.
 * radio_devices_docs/wl55_device/arch/store.md */
void device_reopen_enrol(void) {
    enrol_window_from_s = timebase_uptime_s();
    enrol_shut_said = 0;
    invite_last_ms = 0;
}

int device_release_pairing(void) {
    if (store_release_pairing() != 0)
        return -1;
    /* The RAM copy too, or it stays paired until the reset this replaces. */
    memset(&join_res, 0, sizeof(join_res));
    device_reopen_enrol();
    return 0;
}

void device_service(void) {
    if (join_is_paired()) {
        recover_service();
        report_service();
    } else {
        invite_service();
    }
}

int device_pubkey(uint8_t *pub) {
    if (!pairing_pubkey_c(&pair_ctx, pub, X25519_PUB_LEN))
        return -1;
    return 0;
}

void device_snapshot(device_view_t *v) {
    store_state_t st;

    memset(v, 0, sizeof(*v));
    v->provisioned  = (store_init(&st) == 0 && st.valid) ? 1u : 0u;
    v->paired       = (uint8_t)(join_is_paired() ? 1u : 0u);
    v->dev_id       = v->provisioned ? st.dev_id : 0u;
    v->hub_id       = v->provisioned ? st.hub_id : 0u;
    v->net_id       = v->provisioned ? (uint16_t)st.net_id : 0u;
    /* The regime, always: a delivery figure that does not state k has no regime. */
    v->opp          = report_opp;
    v->opp_all      = report_opp_all;
    v->slot         = join_res.slot;
    v->report_every = join_res.report_every;
    v->key_gen      = v->provisioned ? st.key_gen : 0u;
    v->superframe   = superframe_now(&sframe);
    v->period_us    = sframe.measured_us;
    v->since_beacon_us = superframe_since_beacon_us(&sframe);
    v->schedulable  = (uint8_t)(superframe_can_schedule(&sframe) ? 1u : 0u);
    v->recovering   = (uint8_t)(recover_state != RECOVER_IDLE ? 1u : 0u);
    v->recover_entered = recover_entered;
    v->rssi_down_dbm   = beacon_rssi_dbm;
    v->rssi_valid      = beacon_rssi_valid;
    v->tx_floor        = tx_floor;
    v->tx_floor_known  = tx_floor_known;
    {
        /* Only the verifier sees a frame.
         * radio_devices_docs/wl55_device/radio/pairing.md */
        pair_init_stats_t ps;

        pair_init_stats(&ps);
        v->invites_seen = ps.seen;
    }
    {
        /* A counter that stopped moving reads as "still looking" otherwise.
         * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
        uint32_t open_s = timebase_uptime_s() - enrol_window_from_s;
        uint32_t win_s  = DEVICE_ENROL_WINDOW_MS / 1000u;

        v->enrol_open   = (uint8_t)((open_s < win_s) ? 1u : 0u);
        v->enrol_left_s = v->enrol_open ? (win_s - open_s) : 0u;
    }
    v->invite_slices   = invite_slices;
    v->invites_refused = invites_refused;
    v->reports_sent    = reports_sent;
    v->beacons_missed  = beacons_missed;
    v->beacon_windows  = beacon_windows;
    v->downlink_windows = dl_windows;
    v->downlinks_applied = dl_applied;
    v->downlinks_opened  = dl_opened;
    v->downlinks_repeat  = dl_repeats;
    v->downlinks_replay  = dl_replays;
    v->downlink_floor    = dl_floor_known ? dl_floor : 0u;
    v->up_seq            = upseq_spent(&up_seq);
    v->app_len           = dl_app_len;
    v->app_any           = dl_app_any;
    v->app_witness       = dl_app_witness;
    v->app_refused       = dl_app_bad;
    memcpy(v->app, dl_app, sizeof(v->app));
    v->rssi_up_dbm       = hub_rssi_up_dbm;
    v->rssi_up_valid     = hub_rssi_up_valid;
}

void device_init(void) {
    /* The grant lives in flash: a reset must not silently demote a paired device.
     * radio_devices_docs/wl55_device/arch/store.md */
    join_restore(&join_res);
    dl_floor_load();
    tlm_emit(TLM_BOOT, timebase_uptime_s(), (uint32_t)RCC->CSR,
             RADIO_BITRATE_BPS / 1000u, BUILD_ID_RECORD);
    provision_identity();
}
