/* Read-only: every command answers a question and none changes what runs.
 * radio_devices_docs/wl55_device/testing/console.md */
#include "console.h"

#if WL55_CONSOLE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "build_id.h"
#include "crypto.h"
#include "join.h"
#include "device.h"
#include "sensor.h"
#include "hublogic.h"
#include "load.h"
#include "main.h"
#include "pairing.h"
#include "radio.h"
#include "hop.h"
#include "hop_prf.h"
#include "radio_phy.h"
#include "radio_slots.h"
#include "telemetry.h"
#include "clock.h"
#include "vcp.h"
#include "vectors.h"
#if WL55_DEV_COMMANDS
#include "hopref.h"
#endif

#define CON_CMD_LEN   32
#define CON_RESP_LEN  768
#define CON_RX_LEN    64

static char     resp[CON_RESP_LEN];
static int      resp_len;
static char     cmd[CON_CMD_LEN];
static uint8_t  cmd_len;
static volatile uint8_t rx_buf[CON_RX_LEN];
static volatile uint8_t rx_head, rx_tail;

static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(resp + resp_len, (size_t)(CON_RESP_LEN - resp_len), fmt, ap);
    va_end(ap);
    if (n > 0)
        resp_len += (n < CON_RESP_LEN - resp_len) ? n : (CON_RESP_LEN - resp_len);
}

static void flush(void) {
    vcp_write((const uint8_t *)resp, (uint16_t)resp_len, 500);
    resp_len = 0;
}

static void show_state(void) {
    device_view_t v;
    device_snapshot(&v);

    out("build   %s\r\n", BUILD_ID_STR);
    out("uptime  %lu s   reset %08lX\r\n",
        (unsigned long)timebase_uptime_s(), (unsigned long)RCC->CSR);
    out("ident   %s dev %08lX gen %u\r\n",
        v.provisioned ? "stored" : "NONE", (unsigned long)v.dev_id, v.key_gen);
    out("paired  %s", v.paired ? "yes" : "no");
    if (v.paired)
        out("  hub %08lX net %04X slot %u every %u",
            (unsigned long)v.hub_id, v.net_id, v.slot, v.report_every);
    out("\r\n");
    /* Stated, never inferred: run 2026-08-24-1 read k off a duty cycle. */
    if (v.opp_all)
        out("opps    all %u of %u  slots %u/%u/%u\r\n",
            (unsigned)RADIO_SLOT_OPPS, (unsigned)RADIO_SLOT_OPPS,
            (unsigned)v.slot, (unsigned)(v.slot + RADIO_SLOT_STRIDE),
            (unsigned)(v.slot + 2u * RADIO_SLOT_STRIDE));
    else
        out("opps    1 of %u  k=%u  slot %u\r\n", (unsigned)RADIO_SLOT_OPPS,
            (unsigned)v.opp, (unsigned)(v.slot + v.opp * RADIO_SLOT_STRIDE));
    out("clock   sf %lu  period %lu us  since beacon %lu us  %s%s\r\n",
        (unsigned long)v.superframe, (unsigned long)v.period_us,
        (unsigned long)v.since_beacon_us,
        v.schedulable ? "schedulable" : "stale",
        v.recovering ? "  RECOVERING" : "");
    /* Absent rather than zero: an unheard beacon has no level. */
    if (v.rssi_valid)
        out("rssi_down %d dBm\r\n", (int)v.rssi_down_dbm);
    else
        out("rssi_down never heard\r\n");
    /* Absent rather than zero: no downlink yet is not a floor of zero. */
    if (v.tx_floor_known)
        out("tx floor  above %lu (from the hub's last downlink)\r\n",
            (unsigned long)v.tx_floor);
    else
        out("tx floor  none yet - no downlink opened since boot\r\n");
    /* Opened is the denominator; applied counts only the ones that named a command. */
    out("counts  reports %lu  missed %lu  downlinks %lu of %lu  recoveries %lu\r\n",
        (unsigned long)v.reports_sent, (unsigned long)v.beacons_missed,
        (unsigned long)v.downlinks_applied, (unsigned long)v.downlinks_opened,
        (unsigned long)v.recover_entered);
    {
        /* The same numbers the report carries, before the air can be blamed. */
        sensor_reading_t m;
        uint32_t taken, failed;

        sensor_counts(&taken, &failed);
        if (sensor_read(&m) == 0)
            out("sensor  %d.%01u C  %u mV  (%lu taken, %lu failed)\r\n",
                m.temp_c_x10 / 10, (unsigned)(m.temp_c_x10 < 0 ? -m.temp_c_x10
                                              : m.temp_c_x10) % 10u,
                m.supply_mv, (unsigned long)taken, (unsigned long)failed);
        else
            out("sensor  nothing measured yet (%lu taken, %lu failed)\r\n",
                (unsigned long)taken, (unsigned long)failed);
    }
    /* Only `seen` is a denominator.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    out("invites seen %lu  refused %lu  (listening slices %lu)\r\n",
        (unsigned long)v.invites_seen, (unsigned long)v.invites_refused,
        (unsigned long)v.invite_slices);
    /* A slice count alone cannot say whether the node is still looking. ADR-0024 */
    if (v.paired)
        out("enrol   not listening - paired\r\n");
    else if (v.enrol_open)
        out("enrol   listening, window shuts in %lu s\r\n", (unsigned long)v.enrol_left_s);
    else
        out("enrol   WINDOW SHUT - power-cycle or `release` to reopen it\r\n");
}

/* The one console command that writes.
 * radio_devices_docs/wl55_device/arch/store.md */
static void do_release(void) {
    device_view_t v;

    device_snapshot(&v);
    if (!v.paired) {
        out("not paired - nothing to release\r\n");
        return;
    }
    if (device_release_pairing() != 0) {
        out("release failed: the store refused the write, still paired\r\n");
        return;
    }
    /* The id is the point: an erase would have drawn a new one. */
    out("released; identity kept, listening again for %lu s\r\n",
        (unsigned long)(DEVICE_ENROL_WINDOW_MS / 1000u));
}

static void show_ident(void) {
    uint8_t pk[X25519_PUB_LEN];
    device_view_t v;

    device_snapshot(&v);
    if (!v.provisioned) {
        out("no identity in flash\r\n");
        return;
    }
    out("dev %08lX\r\npubkey ", (unsigned long)v.dev_id);
    if (device_pubkey(pk) == 0) {
        for (unsigned i = 0; i < sizeof(pk); i++)
            out("%02X", pk[i]);
    } else {
        out("unavailable");
    }
    /* The id is what the operator types; the key above is only to compare.
     * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
    out("\r\nenrol with: device add %08lX\r\n", (unsigned long)v.dev_id);
}

static void show_radio(void) {
    out("bitrate %lu bps  deviation %lu Hz  bw %lu Hz\r\n",
        (unsigned long)RADIO_BITRATE_BPS, (unsigned long)RADIO_DEVIATION_HZ,
        (unsigned long)RADIO_RX_BW_DEV_HZ);
    out("power   %d dBm commanded\r\n", (int)radio_power_dbm());
    out("grid    from %lu Hz, spacing %lu Hz\r\n",
        (unsigned long)RADIO_CH_BASE_HZ, (unsigned long)RADIO_CH_SPACING_HZ);
    out("slots   %u opportunities, stride %u, slot %lu us, guard %lu us\r\n",
        (unsigned)RADIO_SLOT_OPPS, (unsigned)RADIO_SLOT_STRIDE,
        (unsigned long)RADIO_SLOT_US, (unsigned long)RADIO_SLOT_GUARD_US);
}

static void show_vectors(void) {
    vectors_report_t v;

    vectors_report(&v);
    out("pair_v4 %s\r\nwire_v4 %s\r\n", v.pair_v4, v.wire_v4);
    out("hop shared %s\r\nhop local  %s  %s\r\n", v.hop_shared, v.hop_local,
        v.hop_local_matches_shared ? "agrees" : "DISAGREES");
    out("hop deck   %s  rc %d\r\n",
        v.hop_deck_rc == 0 ? "drawn by this build" : "MISMATCH", v.hop_deck_rc);
#if WL55_DEV_COMMANDS
    /* Printed either way: a verdict seen only on failure is unread. */
    out("hop ref    %s  rc %d\r\n",
        v.hop_ref_rc == 0 ? "the hub's hop.c drew it too" : "MISMATCH",
        v.hop_ref_rc);
    out("hop ctl    %s  rc %d\r\n",
        v.hop_ref_ctl_rc != 0 ? "wrong PRF refused, as it must be"
                              : "VACUOUS - a wrong PRF was accepted",
        v.hop_ref_ctl_rc);
#endif
}

static void show_load(void) {
    uint32_t window = load_window_us();

    out("window %lu us\r\n", (unsigned long)window);
    for (int c = 0; c < LOAD_CATEGORIES; c++) {
        uint32_t us = load_us((load_cat_t)c);
        /* Permille, not percent: the quiet categories round to nothing at 1%. */
        uint32_t per = window ? (uint32_t)(((uint64_t)us * 1000u) / window) : 0u;
        out("%-12s %8lu us  %3lu.%lu%%  calls %lu  worst %lu us\r\n",
            load_name((load_cat_t)c), (unsigned long)us,
            (unsigned long)(per / 10u), (unsigned long)(per % 10u),
            (unsigned long)load_calls((load_cat_t)c),
            (unsigned long)load_max_us((load_cat_t)c));
    }
}

#if WL55_ROLE_HUB
/* The hub role's whole ladder: every stage, so no step fails as a silent zero.
 * radio_devices_docs/radio/phy-seam.md */
static void show_hub(void) {
    hub_view_t h;

    hub_snapshot(&h);
    out("grid    sf %lu  superframes %lu  windows %lu  stall %lu us\r\n",
        (unsigned long)h.frame_counter, (unsigned long)h.superframes,
        (unsigned long)h.windows, (unsigned long)h.stall_us);
    out("tx      invites %lu (err %lu)  beacons %lu\r\n",
        (unsigned long)h.inits_sent, (unsigned long)h.init_tx_err,
        (unsigned long)h.beacons);
    out("rx      sync %lu  crc err %lu  frames %lu  other %lu (last type %02x)\r\n",
        (unsigned long)h.ev_sync, (unsigned long)h.ev_crc,
        (unsigned long)h.ev_frame, (unsigned long)h.other_frames, h.other_type);
    out("req     seen %lu  bad frame %lu  ids %lu  nonce %lu  point %lu  key %lu\r\n",
        (unsigned long)h.req_seen, (unsigned long)h.req_bad_frame,
        (unsigned long)h.req_bad_ids, (unsigned long)h.req_bad_nonce,
        (unsigned long)h.req_bad_point, (unsigned long)h.req_no_key);
    out("rsp     sent %lu  tx err %lu\r\n",
        (unsigned long)h.rsp_sent, (unsigned long)h.rsp_tx_err);
    out("conf    seen %lu  bad frame %lu  ids %lu  no exchange %lu  mismatch %lu\r\n",
        (unsigned long)h.conf_seen, (unsigned long)h.conf_bad_frame,
        (unsigned long)h.conf_bad_ids, (unsigned long)h.conf_no_exchange,
        (unsigned long)h.conf_mismatch);
    out("accept  sent %lu  err %lu  paired %lu  timeouts %lu\r\n",
        (unsigned long)h.accept_sent, (unsigned long)h.accept_tx_err,
        (unsigned long)h.paired, (unsigned long)h.ex_timeouts);
    out("level   request %d dBm  confirm %d dBm\r\n",
        h.req_rssi_dbm, h.conf_rssi_dbm);
    out("timing  i->q %lu  q->s %lu  s->c %lu  c->a %lu us\r\n",
        (unsigned long)h.init_to_req_us, (unsigned long)h.req_to_rsp_us,
        (unsigned long)h.rsp_to_conf_us, (unsigned long)h.conf_to_accept_us);
}
#endif

static void show_help(void) {
    out("state    what the node is doing\r\n");
    out("ident    identity and public key, to enrol on a hub\r\n");
    out("radio    the PHY and grid this build compiled\r\n");
    out("vectors  the test vectors this build was compiled against\r\n");
    out("load     where the CPU's time goes\r\n");
    out("curve    x25519 and the wire vectors, with their cost\r\n");
    out("join     the pairing window's counters and its timing\r\n");
    out("release  drop the pairing, keep the identity, listen again\r\n");
#if WL55_DEV_COMMANDS
    out("opp <k|all>  which of the k opportunities the loop transmits in\r\n");
#endif
#if WL55_ROLE_HUB
    out("hub      the hub role's grid and enrolment ladder\r\n");
#endif
    out("?        this list\r\n");
    /* Saying which commands write is the point of the line. */
#if WL55_DEV_COMMANDS
    out("\n'release' and 'opp' change what the node does; the rest only look."
        "\r\nThis is a WL55_DEV_COMMANDS build - the product build has no 'opp'.\r\n");
#else
    out("\n'release' is the only one that changes anything; the rest only look."
        "\r\nNothing here starts or stops the node.\r\n");
#endif
}

/* The KATs had no caller, which is how a self-test reads as passing.
 * radio_devices_docs/wl55_device/security/self-tests.md */
static void show_curve(void) {
    crypto_x25519_result_t k;
    crypto_wire_result_t w;

    if (crypto_x25519_kat(&k) != 0) {
        out("x25519 kat did not run\r\n");
        return;
    }
    out("rfc7748  public %s  shared %s  low-order refused %s\r\n",
        k.point_ok ? "ok" : "FAIL", k.shared_ok ? "ok" : "FAIL",
        k.reject_ok ? "ok" : "FAIL");
    out("  scalar mult %lu us   ecdh %lu us\r\n",
        (unsigned long)k.mul_us, (unsigned long)k.ecdh_us);

    if (crypto_wire_kat(&w) != 0) {
        out("wire kat did not run\r\n");
        return;
    }
    out("wire_v4 %s  point %s  ecdh %s  session %s  hop %s  ratchet %s\r\n",
        w.digest, w.point_valid ? "ok" : "FAIL", w.ecdh_ok ? "ok" : "FAIL",
        w.session_ok ? "ok" : "FAIL", w.hop_ok ? "ok" : "FAIL",
        w.ratchet_ok ? "ok" : "FAIL");
    out("  gcm seal %s  open %s  forge refused %s  odd %s/%s  low-order %s\r\n",
        w.cipher_ok ? "ok" : "FAIL", w.open_ok ? "ok" : "FAIL",
        w.forge_rejected ? "ok" : "FAIL", w.odd_seal_ok ? "ok" : "FAIL",
        w.odd_open_ok ? "ok" : "FAIL", w.shared_reject_ok ? "ok" : "FAIL");
    out("  total %lu us\r\n", (unsigned long)w.total_us);
}

/* Measured, not assumed: this delay decides whether the hub is listening yet.
 * radio_devices_docs/wl55_device/radio/pairing.md */
static void show_join(void) {
    join_stats_t j;

    join_stats(&j);
    out("req sent %lu  refused rc %u  last type %02x len %u\r\n",
        (unsigned long)j.req_sent, (unsigned)j.invite_refused,
        (unsigned)j.last_type, (unsigned)j.last_len);
    out("timing  invite -> request %lu us  request -> response %lu us"
        "  response -> confirm %lu us\r\n",
        (unsigned long)j.beacon_to_req_us, (unsigned long)j.req_to_rsp_us,
        (unsigned long)j.rsp_to_conf_us);
    out("held    invite -> confirm %lu us  (schedule %lu us)\r\n",
        (unsigned long)j.invite_to_conf_us,
        (unsigned long)(RADIO_PAIR_CONF_REGION * SUPERFRAME_US));
    out("rsp      heard %lu  timeout %lu  crc %lu  skipped %lu (last type %02x)  len %u\r\n",
        (unsigned long)j.rsp_heard, (unsigned long)j.rsp_timeout,
        (unsigned long)j.rsp_crc_err, (unsigned long)j.rsp_skipped,
        (unsigned)j.rsp_other_type, (unsigned)j.rsp_len);
    /* Between heard and conf sent: the refusals that used to leave no trace here.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    out("rsp      bad frame %lu  wrong ids %lu  eph is static %lu  bad point %lu  confirm bad %lu\r\n",
        (unsigned long)j.rsp_bad_frame, (unsigned long)j.rsp_wrong_ids,
        (unsigned long)j.rsp_eph_is_static, (unsigned long)j.rsp_bad_point,
        (unsigned long)j.rsp_confirm_bad);
    out("conf     sent %lu\r\n", (unsigned long)j.conf_sent);
    out("accept   heard %lu  timeout %lu  crc %lu  skipped %lu (last type %02x)\r\n",
        (unsigned long)j.accept_heard, (unsigned long)j.accept_timeout,
        (unsigned long)j.accept_crc_err, (unsigned long)j.accept_skipped,
        (unsigned)j.accept_other_type);
    out("accept   bad frame %lu  outside window %lu  bad tag %lu  paired %lu  store failed %lu\r\n",
        (unsigned long)j.accept_bad_frame, (unsigned long)j.accept_outside_window,
        (unsigned long)j.accept_bad_tag, (unsigned long)j.paired,
        (unsigned long)j.store_failed);
}

#if WL55_DEV_COMMANDS
/* Restores what 08e244e dropped when the reporting loop left cli.c. ROADMAP 77 */
static void do_opp(const char *arg) {
    uint8_t all = (strcmp(arg, "all") == 0) ? 1u : 0u;
    uint8_t k   = 0u;

    if (!all) {
        if (arg[0] < '0' || arg[0] > '9' || arg[1] != '\0') {
            out("opp <k|all>, k is 0..%u\r\n", (unsigned)(RADIO_SLOT_OPPS - 1u));
            return;
        }
        k = (uint8_t)(arg[0] - '0');
    }
    if (device_set_opportunities(k, all) != 0) {
        out("k is 0..%u or all\r\n", (unsigned)(RADIO_SLOT_OPPS - 1u));
        return;
    }
    show_state();
}

/* The other implementation, so hopref.c needs no header of this tree's. */
static int other_channel(void *ctx, uint32_t sf, uint8_t *ch) {
    return hop_channel((hop_ctx_t *)ctx, sf, ch);
}

/* Whether swapping one hop.c for the other is a no-op. ADR-0029 decision 5.
 * radio_devices_docs/radio/hopping.md */
static void do_hopsweep(const char *arg) {
    hop_ctx_t mine;
    uint32_t cycles = 0, checked = 0, bad_sf = 0;
    uint32_t start = 0;
    int8_t rc;
    /* The negative case: a sweep never red reads in neither direction. */
    uint8_t ctl = 0u;

    if (strncmp(arg, "ctl ", 4) == 0) {
        ctl = 1u;
        arg += 4;
    }
    if (arg[0] < '0' || arg[0] > '9') {
        out("hopsweep <cycles> [start-cycle]\r\n");
        return;
    }
    for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
        cycles = cycles * 10u + (uint32_t)(*p - '0');
    for (const char *p = arg; *p; p++)
        if (*p == ' ') {
            for (const char *q = p + 1; *q >= '0' && *q <= '9'; q++)
                start = start * 10u + (uint32_t)(*q - '0');
            break;
        }
    if (cycles == 0u || cycles > 5000u) {
        out("cycles is 1..5000\r\n");
        return;
    }
    if (hop_init(&mine, hop_prf_aes, (void *)vectors_hop_key(),
                 RADIO_HOP_COUNT) != 0) {
        out("hopsweep: this build's hop_init refused\r\n");
        return;
    }
    rc = hopref_sweep(ctl ? hop_prf_swap32 : hop_prf_aes,
                      (void *)vectors_hop_key(),
                      RADIO_HOP_COUNT, other_channel, &mine,
                      start, cycles, &checked, &bad_sf);
    /* The denominator: a green line over an empty sweep is no pass. */
    out("hopsweep%s cycles %lu from %lu  channels compared %lu  rc %d\r\n",
        ctl ? " CONTROL" : "",
        (unsigned long)cycles, (unsigned long)start,
        (unsigned long)checked, rc);
    if (ctl)
        out("%s\r\n", rc != 0 ? "control refused it, as it must"
                                : "VACUOUS - a wrong PRF swept clean");
    else if (rc != 0)
        out("first disagreement at superframe %lu\r\n", (unsigned long)bad_sf);
    else
        out("both implementations agree, and every cycle was a permutation\r\n");
}
#endif

static void dispatch(void) {
    if (cmd_len == 0)
        return;
    if (strcmp(cmd, "state") == 0)        show_state();
    else if (strcmp(cmd, "ident") == 0)   show_ident();
    else if (strcmp(cmd, "radio") == 0)   show_radio();
    else if (strcmp(cmd, "vectors") == 0) show_vectors();
    else if (strcmp(cmd, "load") == 0)    show_load();
    else if (strcmp(cmd, "curve") == 0)   show_curve();
    else if (strcmp(cmd, "join") == 0)    show_join();
    else if (strcmp(cmd, "release") == 0) do_release();
#if WL55_DEV_COMMANDS
    else if (strncmp(cmd, "opp ", 4) == 0) do_opp(cmd + 4);
    else if (strncmp(cmd, "hopsweep ", 9) == 0) do_hopsweep(cmd + 9);
#endif
#if WL55_ROLE_HUB
    else if (strcmp(cmd, "hub") == 0)     show_hub();
#endif
    else if (strcmp(cmd, "?") == 0)       show_help();
    else out("%s? try ?\r\n", cmd);
}

void console_init(void) {
    rx_head = rx_tail = 0;
    cmd_len = resp_len = 0;
    __HAL_UART_ENABLE_IT(&hcom_uart[COM1], UART_IT_RXNE);
    HAL_NVIC_SetPriority(LPUART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(LPUART1_IRQn);
    out("\r\nwl55 device %s  read-only console\r\n>>> ", BUILD_ID_STR);
    flush();
}

void console_poll(void) {
    while (rx_tail != rx_head) {
        uint8_t c = rx_buf[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1u) % CON_RX_LEN);

        if (c == '\r' || c == '\n') {
            cmd[cmd_len] = '\0';
            out("\r\n");
            dispatch();
            out(">>> ");
            flush();
            cmd_len = 0;
        } else if ((c == 0x7F || c == '\b') && cmd_len > 0) {
            cmd_len--;
            vcp_write((const uint8_t *)"\b \b", 3, 100);
        } else if (c >= 0x20 && c < 0x7F && cmd_len < CON_CMD_LEN - 1) {
            cmd[cmd_len++] = (char)c;
            vcp_write(&c, 1, 100);
        }
    }
}

/* Called from LPUART1_IRQHandler; keeps the ISR to a byte push. */
void console_rx_byte(uint8_t b) {
    uint8_t next = (uint8_t)((rx_head + 1u) % CON_RX_LEN);
    if (next != rx_tail) {
        rx_buf[rx_head] = b;
        rx_head = next;
    }
}

#else  /* WL55_CONSOLE */

void console_init(void) { }
void console_poll(void) { }
void console_rx_byte(uint8_t b) { (void)b; }

#endif
