/* Console for device bring-up: dispatch table, bounded output, LPUART1 IRQ RX.
 * radio_devices_docs/wl55_device/testing/console.md */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "cli.h"
#include "timebase.h"
#include "radio.h"
#include "crypto.h"
#include "frame.h"
#include "pairing.h"
#include "store.h"
#include "superframe.h"
#include "hop.h"
#include "load.h"
#include "hop_vectors.h"
#include "vectors.h"
#include "join.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "radio_phy.h"
#include "pair_v3.h"   /* the published PAIR_INIT */
#include "pair_init.h"
#include "beacon.h"
#include "wire_v3.h"
#include "telemetry.h"

#define CLI_CMD_LEN     96
#define CLI_RESP_LEN    768
#define CLI_RX_LEN      128

typedef int (*cli_handler_t)(int argc, char **argv);

typedef struct {
    const char   *name;
    uint8_t       min_args;
    uint8_t       max_args;
    cli_handler_t handler;
    const char   *args;
    const char   *help;
} cli_cmd_t;

extern UART_HandleTypeDef hcom_uart[];

static char     resp[CLI_RESP_LEN];
static int      resp_len;
static char     cmd[CLI_CMD_LEN];
static uint8_t  cmd_len;
static volatile uint8_t rx_buf[CLI_RX_LEN];
static volatile uint8_t rx_head, rx_tail;

/* Console time is charged separately: a cost of the rig, not of the protocol.
 * radio_devices_docs/wl55_device/testing/console.md */
static void uart_tx(uint8_t *data, uint16_t len, uint32_t timeout) {
    load_enter(LOAD_CONSOLE);
    HAL_UART_Transmit(&hcom_uart[COM1], data, len, timeout);
    load_exit();
}

static void out(const char *fmt, ...) {
    va_list ap;
    int room = CLI_RESP_LEN - resp_len - 1;
    if (room <= 0)
        return;
    va_start(ap, fmt);
    int n = vsnprintf(resp + resp_len, (size_t)room, fmt, ap);
    va_end(ap);
    if (n > 0)
        resp_len += (n < room) ? n : room;
}



static int cmd_help(int argc, char **argv);

/* Running stats for the two capture instruments, so both report one shape. */
typedef struct {
    uint32_t n, sum, lo, hi, noise;
} gap_stats_t;

/* The preamble detector fires on noise. Nothing past 32 preamble bytes is
 * physical. */
static int gap_plausible(uint32_t gap, uint8_t len) {
    return gap <= (uint32_t)(39u + len) * RADIO_US_PER_BYTE;
}

static void gap_add(gap_stats_t *g, uint32_t gap) {
    if (g->n == 0u || gap < g->lo) g->lo = gap;
    if (g->n == 0u || gap > g->hi) g->hi = gap;
    g->sum += gap;
    g->n++;
}

static int cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    out("uptime   %lu s (%lu ms since the TIM2 wrap)\r\n",
        (unsigned long)timebase_uptime_s(), (unsigned long)millis_hw());
    out("micros   %lu\r\n", (unsigned long)micros());
    out("clock    %lu Hz\r\n", (unsigned long)HAL_RCC_GetHCLKFreq());
    out("reset    0x%08lX\r\n", (unsigned long)RCC->CSR);
    return 0;
}

static int cmd_rng(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "health") == 0) {
        uint32_t sr = 0;
        int ok = crypto_rng_health(&sr);
        out("RNG_SR 0x%08lX%s%s%s%s  %s\r\n", (unsigned long)sr,
            (sr & RNG_SR_SEIS) ? " SEIS" : "", (sr & RNG_SR_SECS) ? " SECS" : "",
            (sr & RNG_SR_CEIS) ? " CEIS" : "", (sr & RNG_SR_CECS) ? " CECS" : "",
            ok == 0 ? "healthy" : "seed or clock error latched");
        return 0;
    }
    (void)argv;
    int n = (argc > 1) ? atoi(argv[1]) : 4;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        uint32_t v = 0;
        if (radio_rng_word(&v) != 0) {
            out("rng error\r\n");
            return 0;
        }
        out("%08lX%s", (unsigned long)v, (i % 4 == 3) ? "\r\n" : " ");
    }
    if (n % 4)
        out("\r\n");
    return 0;
}

static uint8_t radio_slot = 14u;

static int cmd_radio(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "crc") == 0) {
        if (argc < 3) {
            out("crc is %s; usage: radio crc <off|2|2inv>\r\n", radio_crc_name());
            return 0;
        }
        if (radio_set_crc(argv[2]) != 0) {
            out("usage: radio crc <off|2|2inv>\r\n");
            return 0;
        }
        out("crc %s\r\n", radio_crc_name());
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "preamble") == 0) {
        if (argc < 3) {
            out("preamble is %u bytes; usage: radio preamble <2..32>\r\n",
                (unsigned)radio_preamble_bytes());
            return 0;
        }
        if (radio_set_preamble((uint8_t)strtoul(argv[2], NULL, 10)) != 0) {
            out("usage: radio preamble <2..32>\r\n");
            return 0;
        }
        /* Both directions: only the transmit one may move with this command. */
        out("preamble %u bytes: tx air %lu us, rx air %lu us\r\n",
            (unsigned)radio_preamble_bytes(),
            (unsigned long)radio_tx_air_time_us(RADIO_UPLINK_BYTES),
            (unsigned long)radio_rx_air_time_us(RADIO_UPLINK_BYTES));
        return 0;
    }
    /* Moves transmit and receive together. radio_devices_docs/radio/phy.md */
    if (argc >= 2 && strcmp(argv[1], "offset") == 0) {
        if (argc < 3) {
            out("offset is %ld Hz; usage: radio offset <hz>\r\n",
                (long)radio_freq_offset());
            return 0;
        }
        if (radio_set_freq_offset((int32_t)atoi(argv[2])) != 0) {
            out("usage: radio offset <-100000..100000>\r\n");
            return 0;
        }
        out("offset %ld Hz, join channel now %ld Hz\r\n",
            (long)radio_freq_offset(),
            (long)radio_slot_hz(radio_join_slot()) + (long)radio_freq_offset());
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "power") == 0) {
        if (argc < 3) {
            out("power is %d dBm; usage: radio power <-17..14>\r\n", radio_power_dbm());
            return 0;
        }
        if (radio_set_power(atoi(argv[2])) != 0) {
            out("usage: radio power <-17..14>\r\n");
            return 0;
        }
        out("power %d dBm\r\n", radio_power_dbm());
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "capture") == 0) {
        if (argc < 3) {
            out("capture is %s; usage: radio capture <sync|preamble>\r\n",
                radio_capture_name());
            return 0;
        }
        if (radio_set_capture(argv[2]) != 0) {
            out("usage: radio capture <sync|preamble>\r\n");
            return 0;
        }
        /* The preamble edge is an instrument; only sync is a frame start. */
        out("capture %s%s\r\n", radio_capture_name(),
            strcmp(radio_capture_name(), "sync") == 0
                ? "" : " - alignment falls back to RxDone");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        /* Bench slot is the default; a protocol slot has to be asked for.
         * radio_devices_docs/wl55_device/testing/console.md */
        int want_bench = (argc < 3) || strcmp(argv[2], "bench") == 0;
        if (!want_bench)
            radio_slot = (uint8_t)atoi(argv[2]);
        if ((want_bench ? radio_configure_bench() : radio_configure(radio_slot)) != 0) {
            out("configure failed\r\n");
            return 0;
        }
        /* The constant the PHY is configured from; the literal here read 25. */
        if (want_bench)
            out("gfsk %lu kbps  bench  %lu Hz  sync \"benc\"\r\n",
                (unsigned long)(RADIO_BITRATE_BPS / 1000u),
                (unsigned long)radio_bench_hz());
        else
            out("gfsk %lu kbps  slot %u  %lu Hz  sync \"hell\"\r\n",
                (unsigned long)(RADIO_BITRATE_BPS / 1000u), radio_slot,
                (unsigned long)radio_slot_hz(radio_slot));
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "chan") == 0) {
        radio_slot = (uint8_t)atoi(argv[2]);
        if (radio_set_channel(radio_slot) != 0) {
            out("set channel failed\r\n");
            return 0;
        }
        out("slot %u  %lu Hz\r\n", radio_slot, (unsigned long)radio_slot_hz(radio_slot));
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "tx") == 0) {
        uint32_t air = 0;
        uint8_t len = (uint8_t)strlen(argv[2]);
        int rc = radio_send((const uint8_t *)argv[2], len, &air);
        if (rc == 0)
            /* Includes the TCXO ramp, so this is not time on air and not a duty-cycle term.
             * radio_devices_docs/wl55_device/testing/console.md */
            out("sent %u bytes, %lu us from SetTx to TxDone (TCXO start included)\r\n",
                len, (unsigned long)air);
        else if (rc == -4)
            out("no interrupt from the radio at all\r\n");
        else if (rc == -2)
            out("chip reported timeout instead of TxDone\r\n");
        else
            out("tx failed (%d)\r\n", rc);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "beacon") == 0) {
        /* Refused: a test pattern on the join channel decodes as the hub's join beacon.
         * radio_devices_docs/wl55_device/testing/console.md */
        if (!radio_bench_mode() && radio_slot == radio_join_slot()) {
            out("slot %u is the join channel - bench traffic goes on the bench "
                "frequency. Run \"radio init\" first.\r\n", radio_slot);
            return 0;
        }
        /* Two seconds: 0.43% duty and the hub's superframe. 1 Hz is 0.85% and fails.
         * radio_devices_docs/wl55_device/testing/console.md */
        int count = (argc >= 3) ? atoi(argv[2]) : 10;
        if (count < 1) count = 1;
        if (count > 60) count = 60;
        uint32_t sent = 0;
        for (int i = 0; i < count; i++) {
            uint8_t frame[14];
            for (uint8_t j = 0; j < sizeof(frame); j++)
                frame[j] = (uint8_t)('A' + ((i + j) % 26));
            if (radio_send(frame, sizeof(frame), NULL) == 0)
                sent++;
            delay_us_poll(2000000u);
        }
        out("beacon sent %lu of %d frames at 0.5 Hz\r\n", (unsigned long)sent, count);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "gap") == 0) {
        /* The bench twin of "time capture", with no grid and no beacon. */
        static uint8_t rx[64];
        uint32_t want = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 5u;
        uint32_t tries = 0;
        gap_stats_t g = {0};

        uint32_t asked = want;
        if (want == 0u || want > 32u)
            want = 5u;
        /* A default that stands in for a refused argument must say it did.
         * radio_devices_docs/wl55_device/testing/console.md */
        out("capture %s, %lu frames%s\r\n", radio_capture_name(),
            (unsigned long)want, (asked != want) ? " (1..32; yours refused)" : "");
        while (g.n < want && tries < 4u * want) {
            radio_rx_info_t info = {0};
            info.timeout_us = 5000000u;
            tries++;
            if (radio_receive(rx, sizeof(rx) - 1u, &info) != 0)
                continue;
            if (!info.capture_valid) {
                out("rx but no capture edge\r\n");
                continue;
            }
            uint32_t gap = info.done_us - info.capture_us;
            uint32_t frame_only = ((uint32_t)info.len + 3u) * RADIO_US_PER_BYTE;
            if (!gap_plausible(gap, info.len)) {
                out("gap %lu us - not an edge of this frame  rssi %d dBm\r\n",
                    (unsigned long)gap, info.rssi_dbm);
                g.noise++;
                continue;
            }
            out("len %2u  gap %5lu us  %+ld vs frame-only  rssi %d dBm\r\n",
                (unsigned)info.len, (unsigned long)gap,
                (long)(int32_t)(gap - frame_only), info.rssi_dbm);
            gap_add(&g, gap);
        }
        if (g.n == 0u) {
            out("nothing captured in %lu tries\r\n", (unsigned long)tries);
            return 0;
        }
        out("capture %s: n %lu  mean %lu us  min %lu  max %lu  noise %lu\r\n",
            radio_capture_name(), (unsigned long)g.n, (unsigned long)(g.sum / g.n),
            (unsigned long)g.lo, (unsigned long)g.hi, (unsigned long)g.noise);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "rx") == 0) {
        static uint8_t rx[64];
        radio_rx_info_t info = {0};
        info.timeout_us = (argc >= 3) ? (uint32_t)atoi(argv[2]) * 1000u : 5000000u;
        int rc = radio_receive(rx, sizeof(rx) - 1u, &info);
        if (rc == 0) {
            rx[info.len] = 0;
            out("rx %u bytes  rssi %d dBm  \"%s\"\r\n", info.len, info.rssi_dbm,
                (const char *)rx);
        } else if (rc == -3) {
            out("crc error - a frame arrived but did not survive\r\n");
        } else if (rc == -2) {
            out("chip reported timeout\r\n");
        } else if (rc == -4) {
            out("nothing heard\r\n");
        } else {
            out("receive failed (%d)\r\n", rc);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "tcxo") == 0) {
        if (argc >= 3) {
            uint32_t us = (uint32_t)strtoul(argv[2], NULL, 10);
            if (radio_set_tcxo_us(us) != 0) {
                out("cannot set %lu us\r\n", (unsigned long)us);
                return 0;
            }
        }
        /* Errors stay in the reading: they separate a dead PLL from a quieter receiver.
         * radio_devices_docs/wl55_device/testing/console.md */
        uint16_t err = 0;
        radio_get_error(&err);
        out("tcxo startup %lu us  op_error 0x%04X%s%s\r\n",
            (unsigned long)radio_tcxo_us(), err,
            (err & 0x0020) ? " xosc_start" : "",
            (err & 0x0040) ? " pll_lock" : "");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "rxramp") == 0) {
        /* Sweeping the timeout is the control: a startup ramp stays flat, anything else does not.
         * radio_devices_docs/wl55_device/testing/console.md */
        static const uint32_t probe_us[] = {20000u, 40000u, 80000u, 160000u};
        for (unsigned i = 0; i < sizeof(probe_us) / sizeof(probe_us[0]); i++) {
            uint32_t span = 0;
            if (radio_rx_ramp_probe(probe_us[i], &span) != 0) {
                out("probe at %lu us failed\r\n", (unsigned long)probe_us[i]);
                continue;
            }
            out("timeout %6lu us -> %6lu us measured, ramp %5ld us\r\n",
                (unsigned long)probe_us[i], (unsigned long)span,
                (long)span - (long)probe_us[i]);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "rxlat") == 0) {
        static uint8_t rx[64];
        radio_rx_info_t info = {0};
        uint32_t lead = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 15000u;
        int want = (argc >= 4) ? atoi(argv[3]) : 10;
        if (want < 1) want = 1;
        if (want > 40) want = 40;

        /* Two frames: the sender's period is measured, never assumed to be 2 s.
         * radio_devices_docs/wl55_device/testing/console.md */
        uint32_t anchor_us = 0, period = 0;
        for (int i = 0; i < 2; i++) {
            info.timeout_us = 8000000u;
            if (radio_receive(rx, sizeof(rx), &info) != 0) {
                out("no frame to anchor on - is the other board sending?\r\n");
                return 0;
            }
            if (i == 1)
                period = info.done_us - anchor_us;
            anchor_us = info.done_us;
        }
        if (period < 100000u || period > 5000000u) {
            out("anchor period %lu us is not plausible\r\n", (unsigned long)period);
            return 0;
        }

        uint32_t hits = 0, crc = 0;
        int32_t rssi_sum = 0;
        for (int i = 0; i < want; i++) {
            uint32_t predicted = anchor_us + period;
            /* A generous tail keeps the window from being the thing measured.
             * radio_devices_docs/wl55_device/testing/console.md */
            info.timeout_us = lead + 20000u;
            int rc = radio_receive_at(rx, sizeof(rx), &info, predicted - lead);
            if (rc == 0) {
                hits++;
                rssi_sum += info.rssi_dbm;
                anchor_us = info.done_us;      /* re-anchor, so drift cannot accumulate */
                continue;
            }
            /* CRC failure and silence are different results: marginal receiver against absent one.
             * radio_devices_docs/wl55_device/testing/console.md */
            if (rc == -3)
                crc++;
            anchor_us = predicted;             /* dead reckon through a miss */
        }
        out("lead %lu us: %lu/%d received, %lu crc, mean rssi %ld dBm, period %lu us\r\n",
            (unsigned long)lead, (unsigned long)hits, want, (unsigned long)crc,
            hits ? (long)(rssi_sum / (int32_t)hits) : 0L, (unsigned long)period);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "err") == 0) {
        uint16_t err = 0;
        if (radio_get_error(&err) != 0) {
            out("subghz did not answer\r\n");
            return 0;
        }
        out("op_error 0x%04X%s%s%s%s\r\n", err,
            (err & 0x0001) ? " rc64k_calib" : "",
            (err & 0x0020) ? " xosc_start" : "",
            (err & 0x0040) ? " pll_lock"   : "",
            (err & 0x0100) ? " pa_ramp"    : "");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        uint8_t st = 0;
        if (radio_get_status(&st) != 0) {
            out("subghz did not answer\r\n");
            return 0;
        }
        /* Status byte: bits 6:4 chip mode, 3:1 command status. */
        out("status 0x%02X  mode %u  cmd %u\r\n", st, (st >> 4) & 7, (st >> 1) & 7);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "reg") == 0) {
        unsigned addr = (unsigned)strtoul(argv[2], NULL, 16);
        uint8_t v = 0;
        if (radio_read_reg((uint16_t)addr, &v) != 0) {
            out("read failed\r\n");
            return 0;
        }
        out("0x%04X 0x%02X\r\n", addr, v);
        return 0;
    }
    out("radio init [slot]  - bench frequency by default, or a protocol slot\r\n");
    out("radio chan <slot> - retune\r\n");
    out("radio tx <text>   - send one frame\r\n");
    out("radio rx [ms]     - listen for one frame\r\n");
    out("radio beacon [n]  - send n frames at 0.5 Hz, for SDR capture\r\n");
    out("radio tcxo [us]   - the oscillator startup wait, which is that latency\r\n");
    out("radio rxramp      - how long SetRx takes to make the receiver live\r\n");
    out("radio rxlat <us> <n> - receive n frames entering RX <us> early\r\n");
    out("radio err         - the chip's latched operating errors\r\n");
    out("radio status      - chip mode and last command status\r\n");
    out("radio reg <hex>   - read a SUBGHZ register\r\n");
    return 0;
}

static int cmd_crypto(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "gcm") == 0) {
        crypto_kat_result_t r;
        if (crypto_gcm_kat(&r) != 0) {
            out("aes hardware refused the operation\r\n");
            return 0;
        }
        out("aes-128-gcm  ct %s  tag %s  pt %s  dectag %s\r\n",
            r.ct_ok ? "ok" : "FAIL", r.tag_ok ? "ok" : "FAIL",
            r.pt_ok ? "ok" : "FAIL", r.dec_tag_ok ? "ok" : "FAIL");
        out("             encrypt %lu us  decrypt %lu us  (%u byte payload)\r\n",
            (unsigned long)r.encrypt_us, (unsigned long)r.decrypt_us, 60u);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "p256") == 0) {
        crypto_p256_result_t r;
        if (crypto_p256_kat(&r) != 0) {
            out("pka refused the operation\r\n");
            return 0;
        }
        out("p-256        dG %s  oncurve %s  reject %s\r\n",
            r.point_ok ? "ok" : "FAIL", r.valid_ok ? "ok" : "FAIL",
            r.reject_ok ? "ok" : "FAIL");
        out("             scalarmul %lu us  pointcheck %lu us\r\n",
            (unsigned long)r.mul_us, (unsigned long)r.check_us);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "wire") == 0) {
        crypto_wire_result_t r;
        if (crypto_wire_kat(&r) != 0) {
            out("wire vectors did not run\r\n");
            return 0;
        }
        out("point %s  ecdh-1term %s  session-1term %s  hop-1term %s\r\n",
            r.point_valid ? "ok" : "FAIL", r.ecdh_ok ? "ok" : "FAIL",
            r.session_ok ? "ok" : "FAIL", r.hop_ok ? "ok" : "FAIL");
        out("ratchet %s  cipher %s  tag %s      %lu ms\r\n",
            r.ratchet_ok ? "ok" : "FAIL", r.cipher_ok ? "ok" : "FAIL",
            r.tag_ok ? "ok" : "FAIL", (unsigned long)(r.total_us / 1000u));
        out("open %s  forgery %s  odd-seal %s  odd-open %s\r\n",
            r.open_ok ? "ok" : "FAIL", r.forge_rejected ? "rejected" : "FAIL",
            r.odd_seal_ok ? "ok" : "FAIL", r.odd_open_ok ? "ok" : "FAIL");
        out("decompress %s  parity %s  reject %s   %lu us\r\n",
            r.decompress_ok ? "ok" : "FAIL", r.decompress_parity_ok ? "ok" : "FAIL",
            r.decompress_reject_ok ? "ok" : "FAIL",
            (unsigned long)r.decompress_us);
        out("shared-decomp %s  decomp-ecdh %s  shared-reject %s\r\n",
            r.shared_decomp_ok ? "ok" : "FAIL", r.decomp_ecdh_ok ? "ok" : "FAIL",
            r.shared_reject_ok ? "ok" : "FAIL");
        /* Names what the set covers, not just its version: these keys come from a one-term ECDH.
         * radio_devices_docs/wl55_device/testing/console.md */
        out("vector set v%d (primitives: single-term ECDH)  %s\r\n",
            WIRE_VECTORS_VERSION, r.digest);
        out("pairing keys are two-term; see pair_v1\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "pair") == 0) {
        crypto_pair_result_t r;
        if (crypto_pair_kat(&r) != 0) {
            out("pair vectors did not run\r\n");
            return 0;
        }
        out("salt %s  transcript %s  session %s\r\n",
            r.salt_ok ? "ok" : "FAIL", r.transcript_ok ? "ok" : "FAIL",
            r.session_ok ? "ok" : "FAIL");
        out("confirm-hub %s  confirm-dev %s  eph-is-static %s\r\n",
            r.confirm_hub_ok ? "ok" : "FAIL", r.confirm_dev_ok ? "ok" : "FAIL",
            r.eph_static_rejected ? "ok" : "FAIL");
        out("accept-open %s  hop-key %s  forgery %s  uplink-seal %s\r\n",
            r.accept_open_ok ? "ok" : "FAIL", r.hop_key_ok ? "ok" : "FAIL",
            r.accept_forge_rejected ? "rejected" : "FAIL",
            r.uplink_seal_ok ? "ok" : "FAIL");
        out("after a hostile CRYP config %s  fips-197 %s\r\n",
            r.after_misconfig_ok ? "ok" : "FAIL", r.fips_ok ? "ok" : "FAIL");
        out("vector set pair_v2  %s   %lu us\r\n",
            r.digest, (unsigned long)r.total_us);
        return 0;
    }
    out("crypto gcm    - AES-128-GCM against the NIST vector\r\n");
    out("crypto p256   - PKA scalar multiply and point validation\r\n");
    out("crypto wire   - shared vectors for the primitives, not pairing output\r\n");
    out("crypto pair   - pair_v2 schedule, and PAIR_ACCEPT opened through CRYP\r\n");
    return 0;
}

/* Sealed here must equal the shared vector byte for byte: proves AAD and nonce assembly.
 * radio_devices_docs/wl55_device/testing/console.md */
static pairing_ctx_t pair_ctx;
static frame_ctx_t frame_ctx;
static uint8_t     frame_ready;

/* Latched: a flash that refuses one write refuses the next, and retrying floods.
 * radio_devices_docs/wl55_device/arch/store.md */
static uint8_t reserve_failed;

/* Extends [tx_floor, tx_mark) before the ceiling. Can erase: 22 ms.
 * radio_devices_docs/wl55_device/arch/store.md */
static int counter_topup(uint32_t sf) {
    uint32_t first, mark;

    if (!frame_ready || reserve_failed)
        return -1;
    if (frame_ctx.tx_mark != 0u && (int32_t)(sf - frame_ctx.tx_mark) < 0)
        return 0;
    if (store_reserve_counter(sf, &first, &mark) != 0) {
        reserve_failed = 1;
        return -1;
    }
    frame_set_tx_mark(&frame_ctx, mark);
    return 0;
}

/* Deferred to the gap: an append can erase for 22 ms, longer than a slot. */
static uint8_t rate_unsaved;

/* The device's window, outside frame_ctx so no console command gates it. */
static uint32_t reserve_ceiling;
static uint8_t  reserve_known;

/* More than a cycle, so the extend never falls due inside one. */
#define RESERVE_TOPUP_AHEAD  64u

/* Stalls up to 22 ms, longer than a slot: callers use the gap or the console. */
static int reserve_extend(uint32_t sf) {
    uint32_t first, mark;

    if (reserve_failed)
        return -1;
    if (store_reserve_counter(sf, &first, &mark) != 0) {
        reserve_failed = 1;
        return -1;
    }
    reserve_ceiling = mark;
    reserve_known = 1;
    if (frame_ready)
        frame_set_tx_mark(&frame_ctx, mark);
    return 0;
}

/* The question a transmit may ask at the slot: no flash, no stall. */
static int reserve_covers(uint32_t sf) {
    return reserve_known && (int32_t)(sf - reserve_ceiling) < 0;
}

/* One place reports why a transmit was refused.
 * radio_devices_docs/wl55_device/testing/console.md */
static int tx_gate(uint32_t sf) {
    counter_topup(sf);
    if (frame_tx_allowed(&frame_ctx, sf))
        return 1;
    if (reserve_failed)
        out("counter %lu is past the reserved ceiling %lu and flash refused to "
            "extend it. Not transmitting: an unreserved counter is nonce reuse "
            "after the next reboot. Reboot to compact the store.\r\n",
            (unsigned long)sf, (unsigned long)frame_ctx.tx_mark);
    else
        out("counter %lu is outside the reserved window [%lu, %lu)\r\n",
            (unsigned long)sf, (unsigned long)frame_ctx.tx_floor,
            (unsigned long)frame_ctx.tx_mark);
    return 0;
}

static void frame_setup(void) {
    if (!frame_ready) {
        frame_init(&frame_ctx, V_KEY_SESSION0, 0x0000002Au, 0x0001u);
        frame_ready = 1;
    }
}

static superframe_t sframe;
static quiesce_t quiesce;

static void time_start(void) {
    if (sframe.running)
        return;
    store_state_t st;
    uint32_t floor = 0;
    if (store_init(&st) == 0 && st.valid)
        floor = st.counter_mark;
    superframe_start(&sframe, floor, SUPERFRAME_STUB_US, floor);
}

static int8_t beacon_rssi_dbm;
static uint8_t beacon_rssi_valid;

static int join_is_paired(void);
static int hop_channel_live(uint32_t sf, uint8_t *ch);

#define DOWNLINK_RX_LEAD_US  8000u
_Static_assert(RADIO_DOWNLINK_RX_OPEN_US - DOWNLINK_RX_LEAD_US >
                   RADIO_AIR_START_TO_END_US(sizeof(radio_data_beacon_t)),
               "receive lead would open inside the beacon and eat it instead");

/* How long a receive stays open for a beacon that should already be there. */
#define REPORT_BEACON_WINDOW_US   40000u

static int cmd_time(int argc, char **argv) {
    time_start();

    if (argc >= 2 && strcmp(argv[1], "beacon") == 0) {
        /* Built from the shared struct, never a local copy.
         * radio_devices_docs/wl55_device/testing/console.md */
        radio_data_beacon_t b;
        b.type       = RADIO_FRAME_DATA_BEACON;
        /* A forgeable version, so the forward-compatibility gate is shown to reject.
         * radio_devices_docs/wl55_device/testing/console.md */
        b.version    = (argc >= 4) ? (uint8_t)atoi(argv[3]) : 2u;
        b.net_id     = 0x0001u;
        b.hub_id     = 0xA7C31E55u;
        b.flags      = 0u;
        b.resume_in  = 0u;
        /* An explicit counter lets one board forge a beacon and take the refusal paths.
         * radio_devices_docs/wl55_device/testing/console.md */
        uint32_t forged = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0u;
        b.superframe = superframe_now(&sframe);
        /* On the boundary, as the hub does: off-boundary makes elapsed/frames not the period.
         * radio_devices_docs/wl55_device/testing/console.md */
        while (!timebase_elapsed(sframe.next_boundary_us))
            ;
        b.superframe = forged ? forged : superframe_now(&sframe);
        if (radio_send((const uint8_t *)&b, (uint8_t)sizeof(b), NULL) != 0) {
            out("beacon not sent\r\n");
            return 0;
        }
        out("beacon superframe %lu, %u bytes\r\n",
            (unsigned long)b.superframe, (unsigned)sizeof(b));
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "quiesce") == 0) {
        /* Forging is the only way to take the clamp, gap and sync refusal paths without a hub.
         * radio_devices_docs/wl55_device/testing/console.md */
        radio_data_beacon_t b;
        b.type       = RADIO_FRAME_DATA_BEACON;
        b.version    = 2u;
        b.net_id     = 0x0001u;
        b.hub_id     = 0xA7C31E55u;
        b.flags      = RADIO_BEACON_FLAG_QUIESCE;
        b.resume_in  = (argc >= 3) ? (uint8_t)atoi(argv[2]) : RADIO_QUIESCE_SUPERFRAMES;
        while (!timebase_elapsed(sframe.next_boundary_us))
            ;
        b.superframe = superframe_now(&sframe);
        if (radio_send((const uint8_t *)&b, (uint8_t)sizeof(b), NULL) != 0) {
            out("announcement not sent\r\n");
            return 0;
        }
        out("quiesce announced at %lu, resume_in %u\r\n",
            (unsigned long)b.superframe, b.resume_in);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "camp") == 0) {
        /* Park and wait: every channel carries one data beacon per cycle, so the wait is bounded.
         * radio_devices_docs/wl55_device/testing/console.md */
        uint8_t rx[32];
        radio_rx_info_t info = {0};
        uint8_t grid = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 0u;
        if (grid == radio_join_slot()) {
            out("grid %u is the join channel and carries no data beacon\r\n", grid);
            return 0;
        }
        if (radio_configure(grid) != 0) {
            out("configure failed\r\n");
            return 0;
        }
        out("camped on grid %u (%lu Hz), worst gap is %u superframes\r\n",
            (unsigned)grid, (unsigned long)radio_slot_hz(grid),
            2u * HOP_VECTORS_COUNT - 1u);
        uint32_t t0 = micros();
        /* Two cycles: once per cycle bounds the count, not the gap, so the worst gap is 2N-1.
         * radio_devices_docs/wl55_device/testing/console.md */
        info.timeout_us = (2u * (uint32_t)HOP_VECTORS_COUNT) * SUPERFRAME_US;
        if (radio_receive(rx, sizeof(rx), &info) != 0) {
            out("no data beacon in two full cycles - the hub is silent or this "
                "is not its grid\r\n");
            return 0;
        }
        uint32_t waited = micros() - t0;
        uint32_t before = superframe_now(&sframe);
        uint32_t aligned = 0;
        /* beacon_apply timestamps after the SPI reads; done_us is the IRQ instant.
         * radio_devices_docs/wl55_device/radio/timebase.md */
        uint32_t stamp_lag = micros() - info.done_us;   /* measured 217-282 us */
        beacon_rc_t rc = beacon_apply(rx, info.len, &sframe, &quiesce, info.start_us, &aligned);
        if (rc != BEACON_OK) {
            /* The number as well as the verdict: the sign is where this went wrong.
             * radio_devices_docs/wl55_device/radio/timebase.md */
            out("heard %u bytes but rejected: %s (claimed %lu, %+ld from here)\r\n",
                info.len, beacon_rc_name(rc), (unsigned long)sframe.refused_counter,
                (long)sframe.refused_jump);
            return 0;
        }
        beacon_rssi_dbm = info.rssi_dbm;
        beacon_rssi_valid = 1;
        /* The cost, not the error: what the grid is no longer charged since done_us.
         * radio_devices_docs/wl55_device/radio/timebase.md */
        out("post-RxDone SPI %lu us, excluded from the alignment\r\n",
            (unsigned long)stamp_lag);
        /* Two estimates of one instant; the gap is wait_irq's poll latency. */
        if (info.capture_valid && info.capture_sync)
            out("frame start: RxDone-derived %+ld us against the sync ISR\r\n",
                (long)(int32_t)((info.done_us - RADIO_AIR_START_TO_END_US(info.len)) -
                                (info.capture_us - RADIO_AIR_START_TO_SYNC_US)));
        else if (info.capture_valid)
            out("frame start: capture is the preamble edge, not a frame start\r\n");
        else
            out("frame start: no capture interrupt was taken\r\n");
        out("aligned to %lu from %lu (%+ld) after %lu ms  rssi %d dBm\r\n",
            (unsigned long)aligned, (unsigned long)before,
            (long)(int32_t)(aligned - before), (unsigned long)(waited / 1000u),
            info.rssi_dbm);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "joinsync") == 0) {
        /* Aligned here, before the exchange: five seconds of handshake is two superframes.
         * radio_devices_docs/wl55_device/radio/timebase.md */
        uint8_t rx[32];
        radio_rx_info_t info = {0};
        radio_join_beacon_t b;
        info.timeout_us = (argc >= 3) ? (uint32_t)atoi(argv[2]) * 1000u : 8000000u;
        if (radio_configure(radio_join_slot()) != 0) {
            out("configure failed\r\n");
            return 0;
        }
        if (radio_receive(rx, sizeof(rx), &info) != 0) {
            out("no join beacon heard - is the hub's window open?\r\n");
            return 0;
        }
        if (info.len != sizeof(b) || rx[0] != RADIO_FRAME_JOIN_BEACON ||
            rx[1] != RADIO_PROTO_VERSION) {
            out("not a join beacon: %u bytes, type %02X, version %02X\r\n",
                info.len, info.len ? rx[0] : 0u, info.len > 1u ? rx[1] : 0u);
            return 0;
        }
        memcpy(&b, rx, sizeof(b));
        uint32_t before = superframe_now(&sframe);
        superframe_align(&sframe, b.superframe);
        out("aligned to %lu from %lu (%+ld)  rssi %d dBm\r\n",
            (unsigned long)b.superframe, (unsigned long)before,
            (long)(int32_t)(b.superframe - before), info.rssi_dbm);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "capture") == 0) {
        /* capture -> RxDone. The difference between the two sources is the
         * sender's preamble.
         * radio_devices_docs/wl55_device/radio/timebase.md */
        uint8_t rx[32], hop, grid;
        uint32_t want = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 8u;
        uint32_t missed = 0, tries = 0;
        gap_stats_t g = {0};

        if (!join_is_paired() || sframe.measured_us == 0u) {
            out("needs an aligned device - run \"time follow\" until it aligns\r\n");
            return 0;
        }
        uint32_t asked = want;
        if (want == 0u || want > 64u)
            want = 8u;
        out("capture %s, %lu beacons%s\r\n", radio_capture_name(),
            (unsigned long)want, (asked != want) ? " (1..64; yours refused)" : "");
        while (g.n < want && tries < 3u * want) {
            radio_rx_info_t info = {0};
            uint32_t aligned = 0;
            uint32_t sf = superframe_now(&sframe) + 1u;
            uint32_t boundary = sframe.next_boundary_us;
            tries++;
            if (hop_channel_live(sf, &hop) != 0 ||
                radio_configure((grid = hop_to_grid(hop))) != 0) {
                out("hop or configure failed\r\n");
                return 0;
            }
            while (!timebase_elapsed(boundary - DOWNLINK_RX_LEAD_US)) { }
            info.timeout_us = REPORT_BEACON_WINDOW_US;
            if (radio_receive(rx, sizeof(rx), &info) != 0 ||
                beacon_apply(rx, info.len, &sframe, &quiesce, info.start_us,
                             &aligned) != BEACON_OK) {
                missed++;
                continue;
            }
            if (!info.capture_valid) {
                out("grid %u: RxDone but no capture edge\r\n", (unsigned)grid);
                continue;
            }
            uint32_t gap = info.done_us - info.capture_us;
            /* The gap if the edge were sync and only the frame followed. */
            uint32_t frame_only = ((uint32_t)info.len + 3u) * RADIO_US_PER_BYTE;
            if (!gap_plausible(gap, info.len)) {
                out("grid %2u  gap %lu us - not an edge of this frame\r\n",
                    (unsigned)grid, (unsigned long)gap);
                g.noise++;
                continue;
            }
            out("grid %2u  len %2u  gap %5lu us  %+ld vs frame-only\r\n",
                (unsigned)grid, (unsigned)info.len, (unsigned long)gap,
                (long)(int32_t)(gap - frame_only));
            gap_add(&g, gap);
        }
        if (g.n == 0u) {
            out("no beacon captured in %lu tries\r\n", (unsigned long)tries);
            return 0;
        }
        out("capture %s: n %lu  mean %lu us  min %lu  max %lu  missed %lu  "
            "noise %lu\r\n", radio_capture_name(), (unsigned long)g.n,
            (unsigned long)(g.sum / g.n), (unsigned long)g.lo, (unsigned long)g.hi,
            (unsigned long)missed, (unsigned long)g.noise);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "follow") == 0) {
        uint8_t rx[32];
        radio_rx_info_t info = {0};
        info.timeout_us = (argc >= 3) ? (uint32_t)atoi(argv[2]) * 1000u : 8000000u;
        /* Needs an aligned counter, and says which channel it listened on.
         * radio_devices_docs/wl55_device/testing/console.md */
        if (join_is_paired()) {
            uint8_t hch, hgrid;
            uint32_t sf_next = superframe_now(&sframe) + 1u;
            if (hop_channel_live(sf_next, &hch) != 0) {
                out("hop failed\r\n");
                return 0;
            }
            hgrid = hop_to_grid(hch);
            if (radio_configure(hgrid) != 0) {
                out("configure failed\r\n");
                return 0;
            }
            out("listening on grid %u (%lu Hz) for superframe %lu\r\n",
                (unsigned)hgrid, (unsigned long)radio_slot_hz(hgrid),
                (unsigned long)sf_next);
        }
        if (radio_receive(rx, sizeof(rx), &info) != 0) {
            out("no beacon heard\r\n");
            return 0;
        }
        uint32_t before = superframe_now(&sframe);
        uint32_t aligned = 0;
        beacon_rc_t rc = beacon_apply(rx, info.len, &sframe, &quiesce, info.start_us, &aligned);
        if (rc != BEACON_OK) {
            out("beacon rejected: %s", beacon_rc_name(rc));
            if (rc == BEACON_BAD_VERSION)
                out(" (%u, this build speaks 2)", info.len >= 2u ? rx[1] : 0u);
            if (rc == BEACON_BAD_LENGTH)
                out(" (%u bytes, v2 is %u)", info.len,
                    (unsigned)sizeof(radio_data_beacon_t));
            out("\r\n");
            return 0;
        }
        beacon_rssi_dbm = info.rssi_dbm;
        beacon_rssi_valid = 1;
        out("aligned to %lu from %lu (%+ld)  rssi %d dBm\r\n",
            (unsigned long)aligned, (unsigned long)before,
            (long)(int32_t)(aligned - before), info.rssi_dbm);
        if (quiesce.active)
            out("quiesced: no uplink until superframe %lu, %lu away\r\n",
                (unsigned long)quiesce.resume_at,
                (unsigned long)(quiesce.resume_at - superframe_now(&sframe)));
        return 0;
    }

    out("transmit gate: %s\r\n",
        superframe_can_schedule(&sframe) ? "open" :
        superframe_is_fresh(&sframe) ? "closed - period is the stub, needs two beacons" :
        "closed - no fresh beacon");
    out("superframe %lu  period %lu us%s  %lu ms since a beacon\r\n",
        (unsigned long)superframe_now(&sframe), (unsigned long)sframe.period_us,
        sframe.measured_us ? " (measured)" : " (stub)",
        (unsigned long)(superframe_since_beacon_us(&sframe) / 1000u));
    if (quiesce_active(&quiesce, superframe_now(&sframe)))
        out("quiesced until %lu - uplink suspended\r\n",
            (unsigned long)quiesce.resume_at);
    if (quiesce.clamped || quiesce.refused_gap || quiesce.refused_sync)
        out("quiesce refusals: %lu clamped  %lu too soon  %lu unsynced\r\n",
            (unsigned long)quiesce.clamped, (unsigned long)quiesce.refused_gap,
            (unsigned long)quiesce.refused_sync);
    out("sync: %s%s", superframe_state_name(&sframe),
        (sframe.state == SF_SYNC_OK && !superframe_is_fresh(&sframe))
            ? " <- STALE, the counter above is a guess" : "");
    if (sframe.rejected)
        out("  (%lu refused since the last good one; last was %lu, %+ld from "
            "here)", (unsigned long)sframe.rejected,
            (unsigned long)sframe.refused_counter, (long)sframe.refused_jump);
    out("\r\n");
    out("time beacon [n [v]] - transmit a data beacon, playing the hub\r\n");
    out("time follow [ms]   - hear one and align to it\r\n");
    return 0;
}

static int cmd_frame(int argc, char **argv) {
    static uint8_t buf[FRAME_MAX_LEN];
    static uint8_t plain[FRAME_MAX_PAYLOAD];

    frame_setup();

    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        frame_ctx.superframe = 123456u;
        uint16_t n = frame_seal(&frame_ctx, FRAME_DIR_UPLINK, 7u,
                                V_PLAIN, sizeof(V_PLAIN), buf);
        if (n == 0) {
            out("seal failed\r\n");
            return 0;
        }
        int hdr_ok = (memcmp(buf, V_AAD, FRAME_HEADER_LEN) == 0);
        int ct_ok  = (memcmp(buf + FRAME_HEADER_LEN, V_CIPHER, sizeof(V_CIPHER)) == 0);
        int tag_ok = (memcmp(buf + FRAME_HEADER_LEN + sizeof(V_CIPHER),
                             V_TAG, FRAME_TAG_LEN) == 0);
        out("sealed %u bytes  header %s  cipher %s  tag %s\r\n", n,
            hdr_ok ? "ok" : "FAIL", ct_ok ? "ok" : "FAIL", tag_ok ? "ok" : "FAIL");

        uint8_t len = 0;
        frame_ctx.have_accepted = 0;
        int rc = frame_open(&frame_ctx, buf, n, 123456u, FRAME_DIR_UPLINK, 7u,
                            plain, &len);
        int rt_ok = (rc == 0) && (len == sizeof(V_PLAIN)) &&
                    (memcmp(plain, V_PLAIN, sizeof(V_PLAIN)) == 0);

        buf[FRAME_HEADER_LEN] ^= 0x01u;   /* flip one ciphertext bit */
        int forged = frame_open(&frame_ctx, buf, n, 123457u, FRAME_DIR_UPLINK, 7u,
                                plain, &len);
        buf[FRAME_HEADER_LEN] ^= 0x01u;

        int replay = frame_open(&frame_ctx, buf, n, 123456u, FRAME_DIR_UPLINK, 7u,
                                plain, &len);
        out("roundtrip %s  forgery %s  replay %s\r\n",
            rt_ok ? "ok" : "FAIL", (forged == -2) ? "rejected" : "FAIL",
            (replay == -3) ? "rejected" : "FAIL");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "key") == 0) {
        if (!pair_ctx.paired) {
            out("not paired - run pair hub or pair device first\r\n");
            return 0;
        }
        /* dev_id goes into the nonce: a mismatch fails the tag exactly like a wrong key.
         * radio_devices_docs/wl55_device/testing/console.md */
        frame_init(&frame_ctx, pair_ctx.session, pair_ctx.dev_id, pair_ctx.net_id);
        store_state_t st;
        /* Key and floor in one record: the old floor belongs to a dead nonce space.
         * radio_devices_docs/wl55_device/arch/store.md */
        if (store_save_session(pair_ctx.session) != 0) {
            /* Reported: the key works until the next reset, then the stale floor makes it deaf.
             * radio_devices_docs/wl55_device/arch/store.md */
            out("could not persist the session key - refusing to install it. "
                "It would work until the next reset and then be gone.\r\n");
            return 0;
        }
        if (store_init(&st) == 0 && st.valid) {
            uint32_t first = st.counter_mark, mark = 0;
            if (store_reserve_counter(superframe_now(&sframe), &first, &mark) != 0) {
                out("could not reserve counter space - refusing to install the "
                    "key rather than seal under counters flash has not kept.\r\n");
                return 0;
            }
            frame_set_floors(&frame_ctx, first, st.rx_floor);
            frame_set_tx_mark(&frame_ctx, mark);
        }
        reserve_failed = 0;
        frame_ready = 1;
        out("frame key set from pairing, dev %08lX  tx floor %lu  rx floor %lu  "
            "key gen %lu\r\n",
            (unsigned long)pair_ctx.dev_id, (unsigned long)frame_ctx.tx_floor,
            (unsigned long)frame_ctx.last_accepted, (unsigned long)st.key_gen);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "lens") == 0) {
        static uint8_t src[FRAME_MAX_PAYLOAD];
        for (uint8_t i = 0; i < sizeof(src); i++)
            src[i] = (uint8_t)(i * 7u + 1u);
        out("failing lengths:");
        int bad = 0;
        for (uint8_t len = 1; len <= 32u; len++) {
            frame_ctx.superframe = 1000u + len;
            frame_ctx.have_accepted = 0;
            uint16_t n = frame_seal(&frame_ctx, FRAME_DIR_UPLINK, 7u, src, len, buf);
            uint8_t got = 0;
            int rc = (n == 0) ? -9
                   : frame_open(&frame_ctx, buf, n, 1000u + len, FRAME_DIR_UPLINK,
                                7u, plain, &got);
            if (rc != 0 || got != len || memcmp(plain, src, len) != 0) {
                out(" %u", len);
                bad++;
            }
        }
        out("%s\r\n", bad ? "" : " none");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "send") == 0) {
        time_start();
        frame_ctx.superframe = superframe_now(&sframe);
        /* Asked before the frame is built, so the refusal reason survives.
         * radio_devices_docs/wl55_device/testing/console.md */
        if (!tx_gate(frame_ctx.superframe))
            return 0;
        uint8_t len = (uint8_t)strlen(argv[2]);
        uint16_t n = frame_seal(&frame_ctx, FRAME_DIR_UPLINK, 7u,
                                (const uint8_t *)argv[2], len, buf);
        if (n == 0) {
            out("refused: the payload does not fit\r\n");
            return 0;
        }
        if (radio_send(buf, (uint8_t)n, NULL) != 0) {
            out("radio would not send\r\n");
            return 0;
        }
        out("sent superframe %lu, %u bytes on air for %u of payload\r\n",
            (unsigned long)frame_ctx.superframe, n, len);
        out("wire:");
        for (uint16_t i = 0; i < n && i < 48u; i++)
            out(" %02X", buf[i]);
        out("\r\n");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "recv") == 0) {
        time_start();
        radio_rx_info_t info = {0};
        info.timeout_us = (argc >= 3) ? (uint32_t)atoi(argv[2]) * 1000u : 10000000u;
        int rc = radio_receive(buf, sizeof(buf), &info);
        if (rc != 0) {
            out("nothing received (%d)\r\n", rc);
            return 0;
        }
        uint8_t len = 0;
        uint32_t now = superframe_now(&sframe);
        /* A small window stands in for the slot schedule; the replay check still gates.
         * radio_devices_docs/wl55_device/testing/console.md */
        static const int8_t window[] = {0, -1, 1};
        uint32_t sf = now;
        rc = -2;
        for (unsigned w = 0; w < sizeof(window) && rc == -2; w++) {
            sf = now + (uint32_t)(int32_t)window[w];
            rc = frame_open(&frame_ctx, buf, info.len, sf, FRAME_DIR_UPLINK, 7u,
                            plain, &len);
        }
        if (rc == 0) {
            plain[len] = 0;
            if (store_note_received(sf) != 0)
                out("warning: the replay floor was not stored, so a reset "
                    "reopens the window below %lu\r\n", (unsigned long)sf);
            out("opened %u bytes at superframe %lu (%+d from local)  rssi %d dBm  "
                "\"%s\"\r\n", len, (unsigned long)sf, (int)(int32_t)(sf - now),
                info.rssi_dbm, (const char *)plain);
        } else if (rc == -2) {
            out("tag failed - wrong key, wrong counter or a forgery\r\n");
            out("got %u bytes:", info.len);
            for (uint8_t i = 0; i < info.len && i < 48u; i++)
                out(" %02X", buf[i]);
            out("\r\n");
        } else if (rc == -3) {
            out("replay - counter not ahead of the last accepted\r\n");
        } else {
            out("malformed frame (%u bytes)\r\n", info.len);
        }
        return 0;
    }

    out("frame key               - use the session key from pairing\r\n");
    out("frame test              - seal against the shared vector, then open it\r\n");
    out("frame send <text>       - seal at the current superframe and transmit\r\n");
    out("frame recv [ms]         - receive and open at the current superframe\r\n");
    return 0;
}

static void pair_show(void) {
    uint8_t fp[SHA256_LEN];
    uint8_t pk[P256_PUB_COMPRESSED_LEN];
    /* Adjacent to the value it checks, so the two are not confused.
     * radio_devices_docs/wl55_device/testing/console.md */
    if (pairing_pubkey_c(&pair_ctx, pk, sizeof(pk))) {
        out("pubkey  ");
        for (unsigned i = 0; i < sizeof(pk); i++) out("%02X", pk[i]);
        out("\r\n");
    }
    if (pairing_fingerprint(&pair_ctx, fp, sizeof(fp))) {
        /* All 32 bytes: a prefix would authenticate a prefix.
         * radio_devices_docs/wl55_device/testing/console.md */
        out("fingerprint ");
        for (int i = 0; i < SHA256_LEN; i++) out("%02X", fp[i]);
        out("\r\n");
    }
    if (pair_ctx.paired) {
        out("session ");
        for (int i = 0; i < 16; i++) out("%02X", pair_ctx.session[i]);
        out("\r\nhop     ");
        for (int i = 0; i < 16; i++) out("%02X", pair_ctx.hop[i]);
        out("\r\n");
    }
}

/* Identity comes off flash: a keypair redrawn per reset is not an identity.
 * radio_devices_docs/wl55_device/testing/console.md */
static void pair_load_identity(void) {
    static uint8_t tried;
    store_state_t st;
    if (tried || pair_ctx.have_key)
        return;
    tried = 1;
    if (store_init(&st) == 0 && st.valid) {
        memcpy(pair_ctx.priv, st.priv, sizeof(pair_ctx.priv));
        if (crypto_p256_public_from_private(pair_ctx.priv, pair_ctx.pub) == 0) {
            pair_ctx.have_key = 1;
            pair_ctx.dev_id = st.dev_id;
        }
    }
}

static int cmd_store(int argc, char **argv) {
    store_state_t st;
    uint32_t records = 0, free_slots = 0, total_slots = 0;

    if (argc >= 2 && strcmp(argv[1], "new") == 0) {
        uint8_t priv[P256_PRIV_LEN], pub[P256_PUB_LEN];
        uint32_t id = 0;
        /* Drawn, never sequential: a small id is long zero runs in a cleartext header.
         * radio_devices_docs/wl55_device/testing/console.md */
        for (int i = 0; i < 32; i++) {
            if (radio_rng_word(&id) != 0) {
                out("rng failed\r\n");
                return 0;
            }
            uint8_t b0 = (uint8_t)id, b1 = (uint8_t)(id >> 8);
            uint8_t b2 = (uint8_t)(id >> 16), b3 = (uint8_t)(id >> 24);
            if (b0 != b1 && b1 != b2 && b2 != b3 && b0 != b2 && b0 != b3 && b1 != b3)
                break;
        }
        if (crypto_p256_keygen(priv, pub) != 0) {
            out("keygen failed\r\n");
            return 0;
        }
        if (store_save_identity(priv, id) != 0) {
            out("flash write failed\r\n");
            return 0;
        }
        memcpy(pair_ctx.priv, priv, sizeof(priv));
        memcpy(pair_ctx.pub, pub, sizeof(pub));
        pair_ctx.have_key = 1;
        pair_ctx.dev_id = id;
        out("identity stored, dev %08lX\r\n", (unsigned long)id);
        pair_show();
        return 0;
    }
    /* `store ceiling` is gone: it wrote a counter the live path could not produce.
     * radio_devices_docs/wl55_device/testing/console.md */
    if (argc >= 2 && strcmp(argv[1], "counter") == 0) {
        /* The same door as the loop, or two ceilings disagree. */
        if (reserve_extend(superframe_now(&sframe)) != 0) {
            out("no identity stored, or flash refused to extend\r\n");
            return 0;
        }
        out("counters below %lu are reserved\r\n",
            (unsigned long)reserve_ceiling);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        /* A cost assertion: the append-point bug returned the right answer via a 22 ms erase.
         * radio_devices_docs/wl55_device/arch/store.md */
        uint32_t first = 0, mark = 0, plain_max, torn_max;
        int ok = 1;

        if (store_erase_all() != 0 || store_save_identity((const uint8_t *)"selftest-identity-32-bytes-long!", 0xA5A5A5A5u) != 0) {
            out("selftest: could not prepare the store\r\n");
            return 0;
        }
        load_reset();
        if (store_reserve_counter(0u, &first, &mark) != 0) ok = 0;
        plain_max = load_max_us(LOAD_FLASH);

        if (store_write_torn() != 0) {
            out("selftest: could not place a torn record\r\n");
            return 0;
        }
        load_reset();
        if (store_reserve_counter(0u, &first, &mark) != 0) ok = 0;
        torn_max = load_max_us(LOAD_FLASH);

        /* Erase ~22 ms against program ~97 us: the threshold only has to separate 200x.
         * radio_devices_docs/wl55_device/arch/store.md */
        int erased = (torn_max > 5000u);
        out("selftest: reserve %lu us max, after a torn record %lu us max\r\n",
            (unsigned long)plain_max, (unsigned long)torn_max);
        out("selftest: %s\r\n",
            (ok && !erased) ? "pass - a torn record does not cost a page erase"
                            : "FAIL - one bad record sends the next write through "
                              "the erase path");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "torn") == 0) {
        uint32_t before = 0, after = 0, recs = 0, freeb = 0, totb = 0;
        store_init(&st);
        before = st.counter_mark;
        if (store_write_torn() != 0) {
            out("could not place a torn record\r\n");
            return 0;
        }
        store_init(&st);
        after = st.counter_mark;
        store_stats(&recs, &freeb, &totb);
        out("torn record written with a higher seq; mark %lu -> %lu  %s\r\n",
            (unsigned long)before, (unsigned long)after,
            (before == after) ? "ignored, as a torn write must be"
                              : "ACCEPTED - the scanner trusted a partial record");
        out("%lu valid record(s), %lu slots free (the torn one occupies a slot)\r\n",
            (unsigned long)recs, (unsigned long)freeb);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "erase") == 0) {
        out(store_erase_all() == 0 ? "store erased\r\n" : "erase failed\r\n");
        pair_ctx.have_key = 0;
        return 0;
    }
    store_init(&st);
    store_stats(&records, &free_slots, &total_slots);
    uint32_t legacy_dev = 0;
    int legacy = store_legacy_present(&legacy_dev);
    if (!st.valid) {
        /* Never written and unreadable are different facts; only one is fixed by `store new`.
         * radio_devices_docs/wl55_device/arch/store.md */
        if (legacy)
            out("%d record(s) of the previous format, newest dev %08lX - "
                "unreadable here, not absent; `store new` and re-enrol\r\n",
                legacy, (unsigned long)legacy_dev);
        else
            out("no identity stored\r\n");
    } else {
    if (legacy)
        out("%d record(s) of the previous format are being stepped over\r\n",
            legacy);
    out("dev %08lX  tx mark %lu  rx floor %lu  key gen %lu\r\n",
            (unsigned long)st.dev_id, (unsigned long)st.counter_mark,
            (unsigned long)st.rx_floor, (unsigned long)st.key_gen);
    out("pair-init ceiling %lu (durable)\r\n", (unsigned long)st.init_ceiling);
    /* 0 is said, not printed: no beacon named the network, against a hub id of zero.
     * radio_devices_docs/wl55_device/testing/console.md */
    if (st.hub_id == 0u)
        out("network: not learned yet - no join beacon heard\r\n");
    else
        out("network: hub %08lX  net %04X\r\n",
            (unsigned long)st.hub_id, (unsigned)st.net_id);
    }
    out("%lu record(s), %lu of %lu slots free\r\n",
        (unsigned long)records, (unsigned long)free_slots, (unsigned long)total_slots);
    return 0;
}

/* The published PAIR_INIT through the live verify path; the Z1 seed is what allows it.
 * radio_devices_docs/wl55_device/radio/pairing.md */
static void pair_init_selftest(void) {
    /* Read out of the vector's header: a constant restated here is one this side owns.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    const pair_init_ctx_t ctx = {
        .dev_priv = NULL, .hub_static_c = NULL,
        .hub_id = (uint32_t)PV3_INIT_HEADER[4] | ((uint32_t)PV3_INIT_HEADER[5] << 8) |
                  ((uint32_t)PV3_INIT_HEADER[6] << 16) | ((uint32_t)PV3_INIT_HEADER[7] << 24),
        .dev_id = (uint32_t)PV3_INIT_HEADER[8] | ((uint32_t)PV3_INIT_HEADER[9] << 8) |
                  ((uint32_t)PV3_INIT_HEADER[10] << 16) | ((uint32_t)PV3_INIT_HEADER[11] << 24),
        .net_id = (uint16_t)((uint16_t)PV3_INIT_HEADER[2] |
                             ((uint16_t)PV3_INIT_HEADER[3] << 8)),
    };
    uint8_t key[32], bad[sizeof(PV3_INIT_FRAME)];
    uint32_t sf = 0;

    out("vectors %s digest %s\r\n", "pair_v3", PAIR_V3_VECTORS_DIGEST);
    /* Checks the shared define against the pinned bytes, which can still disagree.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    out("RADIO_PAIR_INIT_VERSION %u vs vector byte %u: %s\r\n",
        (unsigned)RADIO_PAIR_INIT_VERSION, (unsigned)PV3_INIT_HEADER[1],
        PV3_INIT_HEADER[1] == RADIO_PAIR_INIT_VERSION ? "ok" : "MISMATCH");

    pair_init_key_from_z1(PV3_INIT_Z1, ctx.hub_id, ctx.dev_id, key);
    out("K_init: %s\r\n",
        memcmp(key, PV3_INIT_KEY, sizeof(key)) == 0 ? "ok" : "FAIL");

    pair_init_stats_reset();
    pair_init_forget();
    pair_init_test_seed_z1(PV3_INIT_Z1);

    /* The seed stands in for the ECDH; the prefix check still needs a provisioned key.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    pair_init_ctx_t c = ctx;
    static const uint8_t fake_hub[33] = { 0x02 };
    c.hub_static_c = fake_hub;

    pair_init_rc_t rc = pair_init_verify(&c, PV3_INIT_FRAME, sizeof(PV3_INIT_FRAME),
                                         0u, 0u, &sf);
    out("published frame: %s  superframe %08lX %s\r\n", pair_init_rc_name(rc),
        (unsigned long)sf,
        (rc == PAIR_INIT_OK && sf == PAIR_V3_INIT_SUPERFRAME) ? "ok" : "FAIL");

    /* The rate limit: what an unauthenticated frame must not be able to spend.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    rc = pair_init_verify(&c, PV3_INIT_FRAME_NEXT_SF, sizeof(PV3_INIT_FRAME_NEXT_SF),
                          1u, sf, NULL);
    out("straight after: %s %s\r\n", pair_init_rc_name(rc),
        rc == PAIR_INIT_RATE_LIMITED ? "ok" : "FAIL");

    /* Past the gap, same superframe as the one already accepted. */
    rc = pair_init_verify(&c, PV3_INIT_FRAME, sizeof(PV3_INIT_FRAME),
                          60000u, sf, NULL);
    out("replayed frame: %s %s\r\n", pair_init_rc_name(rc),
        rc == PAIR_INIT_REPLAY ? "ok" : "FAIL");

    /* The control for the replay refusal above: it must still pass.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    uint32_t sf2 = 0;
    rc = pair_init_verify(&c, PV3_INIT_FRAME_NEXT_SF, sizeof(PV3_INIT_FRAME_NEXT_SF),
                          60000u, sf, &sf2);
    out("next superframe: %s  %08lX %s\r\n", pair_init_rc_name(rc),
        (unsigned long)sf2,
        (rc == PAIR_INIT_OK && sf2 == PAIR_V3_INIT_SUPERFRAME + 1u) ? "ok" : "FAIL");

    /* The cross-reboot case: `forget` drops exactly the RAM state a power cycle drops.
     * radio_devices_docs/wl55_device/arch/store.md */
    pair_init_forget();
    pair_init_test_seed_z1(PV3_INIT_Z1);
    rc = pair_init_verify(&c, PV3_INIT_FRAME, sizeof(PV3_INIT_FRAME),
                          0u, PAIR_V3_INIT_SUPERFRAME, NULL);
    out("after a reboot: %s %s\r\n", pair_init_rc_name(rc),
        rc == PAIR_INIT_REPLAY ? "ok" : "FAIL");

    /* One flipped MAC bit, everything else the published frame. */
    memcpy(bad, PV3_INIT_FRAME, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0x01u;
    rc = pair_init_verify(&c, bad, sizeof(bad), 120000u, 0u, NULL);
    out("flipped MAC:    %s %s\r\n", pair_init_rc_name(rc),
        rc == PAIR_INIT_BAD_MAC ? "ok" : "FAIL");

    /* Fresh state, so neither the rate limit nor the ceiling is what makes this pass.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    pair_init_forget();
    pair_init_test_seed_z1(PV3_INIT_Z1);
    pair_init_ctx_t blank = ctx;
    blank.hub_id = 0u; blank.net_id = 0u; blank.hub_static_c = fake_hub;
    rc = pair_init_verify(&blank, PV3_INIT_FRAME, sizeof(PV3_INIT_FRAME),
                          0u, 0u, NULL);
    out("network unknown: %s %s\r\n", pair_init_rc_name(rc),
        rc == PAIR_INIT_OK ? "ok" : "FAIL");

    /* One bit flipped in the hub id changes the salt: adoption is safe, not permissive.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    memcpy(bad, PV3_INIT_FRAME, sizeof(bad));
    bad[4] ^= 0x01u;
    pair_init_forget();
    pair_init_test_seed_z1(PV3_INIT_Z1);
    rc = pair_init_verify(&blank, bad, sizeof(bad), 0u, 0u, NULL);
    out("forged hub id:  %s %s\r\n", pair_init_rc_name(rc),
        rc == PAIR_INIT_BAD_MAC ? "ok" : "FAIL");

    pair_init_stats_t st;
    pair_init_stats(&st);
    /* The cost: Z1 is a 103 ms scalar multiply an attacker buys for 28 bytes.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    out("Z1 derivations %lu - seeded throughout, so nothing above covers the "
        "ECDH; `pair init build` is where that path runs\r\n",
        (unsigned long)st.z1_derivations);
    pair_init_forget();
}

static int parse_hex(const char *in, uint8_t *out, uint32_t out_len);

/* Returns 0 if anything is missing rather than filling a field with a plausible zero.
 * radio_devices_docs/wl55_device/testing/console.md */
static int pair_init_live_ctx(pair_init_ctx_t *c, store_state_t *st) {
    store_init(st);
    /* hub_id 0 is allowed for listening: the ids are inside the MAC salt.
     * radio_devices_docs/wl55_device/radio/pairing.md */
    if (!st->valid)
        return 0;
    c->dev_priv     = st->priv;
    c->hub_static_c = st->hub_static;
    c->dev_id       = st->dev_id;
    c->hub_id       = st->hub_id;
    c->net_id       = st->net_id;
    return 1;
}

static int cmd_pair(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        pair_init_ctx_t c = {0};
        store_state_t st;
        uint8_t f[sizeof(radio_pair_init_t)];

        if (argc >= 3 && strcmp(argv[2], "build") == 0) {
            /* Only this device can build an invitation to itself: that needs Z1, so it needs the key.
             * radio_devices_docs/wl55_device/radio/pairing.md */
            if (!pair_init_live_ctx(&c, &st) || c.hub_id == 0u) {
                out("no identity, hub key or network yet\r\n");
                return 0;
            }
            uint32_t sf = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10)
                                      : superframe_now(&sframe);
            uint8_t key[32], mac[SHA256_LEN];
            const uint32_t hdr = sizeof(f) - RADIO_PAIR_INIT_MAC_LEN;
            f[0] = RADIO_FRAME_PAIR_INIT;
            f[1] = RADIO_PAIR_INIT_VERSION;
            f[2] = (uint8_t)c.net_id;      f[3] = (uint8_t)(c.net_id >> 8);
            f[4] = (uint8_t)c.hub_id;      f[5] = (uint8_t)(c.hub_id >> 8);
            f[6] = (uint8_t)(c.hub_id >> 16); f[7] = (uint8_t)(c.hub_id >> 24);
            f[8] = (uint8_t)c.dev_id;      f[9] = (uint8_t)(c.dev_id >> 8);
            f[10] = (uint8_t)(c.dev_id >> 16); f[11] = (uint8_t)(c.dev_id >> 24);
            f[12] = (uint8_t)sf;           f[13] = (uint8_t)(sf >> 8);
            f[14] = (uint8_t)(sf >> 16);   f[15] = (uint8_t)(sf >> 24);
            if (pair_init_key(&c, key) != 0) {
                out("Z1 failed\r\n");
                return 0;
            }
            hmac_sha256(key, sizeof(key), f, hdr, mac);
            memcpy(f + hdr, mac, RADIO_PAIR_INIT_MAC_LEN);
            out("superframe %lu\r\n", (unsigned long)sf);
            for (uint32_t i = 0; i < sizeof(f); i++) out("%02X", f[i]);
            out("\r\n");
            return 0;
        }
        if (argc >= 4 && strcmp(argv[2], "tx") == 0) {
            if (parse_hex(argv[3], f, sizeof(f)) != 0) {
                out("usage: pair init tx <56 hex digits>\r\n");
                return 0;
            }
            if (radio_configure(radio_join_slot()) != 0 ||
                radio_send(f, sizeof(f), NULL) != 0) {
                out("radio would not send\r\n");
                return 0;
            }
            out("sent %u bytes on grid %u (%lu Hz)\r\n", (unsigned)sizeof(f),
                (unsigned)radio_join_slot(),
                (unsigned long)radio_slot_hz(radio_join_slot()));
            return 0;
        }
        if (argc >= 3 && strcmp(argv[2], "listen") == 0) {
            radio_rx_info_t info = {0};
            uint32_t sf = 0, ceiling = 0;

            if (!pair_init_live_ctx(&c, &st)) {
                out("no identity or hub key yet\r\n");
                return 0;
            }
            ceiling = st.init_ceiling;
            info.timeout_us = ((argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10)
                                           : 10000u) * 1000u;
            if (radio_configure(radio_join_slot()) != 0) {
                out("configure failed\r\n");
                return 0;
            }
            /* Steps past non-invitations, and captures elapsed before radio_receive overwrites it.
             * radio_devices_docs/wl55_device/testing/console.md */
            uint32_t window_us = info.timeout_us;
            uint32_t deadline = micros() + window_us;
            uint32_t others = 0;
            uint8_t  last_other = 0;
            pair_init_rc_t rc = PAIR_INIT_BAD_FRAME;
            for (;;) {
                uint32_t left = deadline - micros();
                if ((int32_t)left <= 0) {
                    out("no invitation on grid %u in %lu ms; %lu other frame(s), "
                        "last type %02X\r\n", (unsigned)radio_join_slot(),
                        (unsigned long)(window_us / 1000u),
                        (unsigned long)others, (unsigned)last_other);
                    return 0;
                }
                info.timeout_us = left;
                if (radio_receive(f, sizeof(f), &info) != 0)
                    continue;
                if (info.len < 1u || f[0] != RADIO_FRAME_PAIR_INIT) {
                    others++;
                    last_other = (info.len >= 1u) ? f[0] : 0u;
                    continue;
                }
                rc = pair_init_verify(&c, f, info.len, millis_hw(), ceiling, &sf);
                out("%u bytes, %d dBm: %s  (%lu other frame(s) first, last type "
                    "%02X)\r\n", (unsigned)info.len, info.rssi_dbm,
                    pair_init_rc_name(rc), (unsigned long)others,
                    (unsigned)last_other);
                break;
            }
            if (rc != PAIR_INIT_OK)
                return 0;
            /* Only after the MAC: a ceiling raised before it locks out every genuine invitation.
             * radio_devices_docs/wl55_device/radio/pairing.md */
            out("superframe %lu, ceiling %lu -> %s\r\n", (unsigned long)sf,
                (unsigned long)ceiling,
                store_save_init_ceiling(sf) == 0 ? "stored" : "FLASH REFUSED");
            /* Learned only from a frame whose MAC verified.
             * radio_devices_docs/wl55_device/radio/pairing.md */
            if (c.hub_id == 0u) {
                uint32_t hub = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
                               ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
                uint16_t net = (uint16_t)((uint16_t)f[2] | ((uint16_t)f[3] << 8));
                out("network adopted from the invitation: hub %08lX net %04X -> %s\r\n",
                    (unsigned long)hub, (unsigned)net,
                    store_save_network(hub, net) == 0 ? "stored" : "FLASH REFUSED");
            }
            return 0;
        }
        pair_init_selftest();
        return 0;
    }
    pair_load_identity();
    if (argc >= 2 && strcmp(argv[1], "keygen") == 0) {
        uint32_t t0 = micros();
        if (pairing_keygen(&pair_ctx) != 0) {
            out("keygen failed\r\n");
            return 0;
        }
        out("keypair generated in %lu ms\r\n",
            (unsigned long)((micros() - t0) / 1000u));
        pair_show();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "salt") == 0) {
        /* Integers, not the packed salt: the little-endian to big-endian step is under test.
         * radio_devices_docs/wl55_device/radio/pairing.md */
        uint8_t sess[16], hop[16];
        pairing_salt_check(V_ECDH_X, 0x33442211u, 0x0000002Au, sess, hop);
        out("salt from ids  session %s  hop %s\r\n",
            memcmp(sess, V_KEY_SESSION0, 16) == 0 ? "ok" : "FAIL",
            memcmp(hop, V_KEY_HOP0, 16) == 0 ? "ok" : "FAIL");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "show") == 0) {
        if (!pair_ctx.have_key)
            out("no keypair yet\r\n");
        pair_show();
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "hub") == 0 || strcmp(argv[1], "device") == 0)) {
        int as_hub = (strcmp(argv[1], "hub") == 0);
        uint32_t ms = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 20000u;
        pair_ctx.net_id = 0x0001u;
        /* Mixed bits by construction: a small id gives long zero runs in the header.
         * radio_devices_docs/wl55_device/testing/console.md */
        pair_ctx.hub_id = 0xA7C31E55u;
        if (!as_hub) {
            pair_ctx.dev_id = 0x5B29D4A1u;
            /* A third argument points at a different hub, so the filter is shown to reject.
             * radio_devices_docs/wl55_device/testing/console.md */
            if (argc >= 4)
                pair_ctx.hub_id = (uint32_t)strtoul(argv[3], NULL, 16);
        }
        uint32_t t0 = micros();
        int rc = as_hub ? pairing_run_hub(&pair_ctx, ms)
                        : pairing_run_device(&pair_ctx, ms);
        uint32_t took = (micros() - t0) / 1000u;
        if (rc == 0) {
            out("paired as %s in %lu ms  hub %08lX  dev %08lX\r\n",
                as_hub ? "hub" : "device", (unsigned long)took,
                (unsigned long)pair_ctx.hub_id, (unsigned long)pair_ctx.dev_id);
            pair_show();
        } else if (rc == -3) {
            out("no peer answered\r\n");
        } else if (rc == -4) {
            out("frame was not a pairing frame\r\n");
        } else if (rc == -5) {
            out("peer public key rejected - not a point on the curve\r\n");
        } else if (rc == -6) {
            out("frame addressed elsewhere - dropped before spending the PKA\r\n");
        } else {
            out("pairing failed (%d)\r\n", rc);
        }
        return 0;
    }
    out("pair keygen        - draw a long-term keypair from the TRNG\r\n");
    out("pair salt          - derive from ids held as integers, not a packed salt\r\n");
    out("pair show          - fingerprint, and the keys if paired\r\n");
    out("pair hub [ms]      - play the hub: listen, answer, derive\r\n");
    out("pair device [ms]   - play the device: ask, wait, derive\r\n");
    return 0;
}

/* Printed, not checked: one build here, so a digest can only disagree elsewhere.
 * radio_devices_docs/wl55_device/testing/console.md */
static int cmd_vectors(int argc, char **argv) {
    vectors_report_t v;
    (void)argc; (void)argv;
    vectors_report(&v);
    out("pair_v2   %s\r\nwire_v3   %s\r\nhop_v1    %s\r\n",
        v.pair_v2, v.wire_v3, v.hop_shared);
    out("hop local %s  values vs hop_v1: %s\r\n",
        v.hop_local, v.hop_local_matches_shared ? "agree" : "DIVERGED");
    return 0;
}



static join_result_t join_res;

/* Exactly 2*len digits or nothing: a short parse derives against a prefix.
 * radio_devices_docs/wl55_device/testing/console.md */
static int parse_hex(const char *s, uint8_t *out, uint32_t len) {
    if (s == NULL || strlen(s) != len * 2u)
        return -1;
    for (uint32_t i = 0; i < len * 2u; i++) {
        char c = s[i];
        uint8_t n;
        if (c >= '0' && c <= '9')      n = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') n = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') n = (uint8_t)(c - 'A' + 10);
        else return -1;
        if (i & 1u) out[i / 2u] = (uint8_t)(out[i / 2u] | n);
        else        out[i / 2u] = (uint8_t)(n << 4);
    }
    return 0;
}

/* The generation separates a grant just negotiated from one restored from flash.
 * radio_devices_docs/wl55_device/testing/console.md */
static void paired_line(void) {
    uint32_t gen = 0;
    out("paired: slot %u  report every %u  ", (unsigned)join_res.slot,
        (unsigned)join_res.report_every);
    /* Not the same statement as generation zero, and they demand opposite reads. */
    if (store_key_gen(&gen) == 0) out("key gen %lu\r\n", (unsigned long)gen);
    else                          out("key gen unreadable\r\n");
}

static int cmd_join(int argc, char **argv) {
    pair_load_identity();
    if (argc >= 2 && strcmp(argv[1], "hub-key") == 0) {
        if (argc < 3 || parse_hex(argv[2], join_res.hub_static,
                                  sizeof(join_res.hub_static)) != 0) {
            out("usage: join hub-key <66 hex digits, compressed SEC1>\r\n");
            return 0;
        }
        if (join_res.hub_static[0] != 0x02u && join_res.hub_static[0] != 0x03u) {
            out("not a compressed point - the first byte must be 02 or 03\r\n");
            return 0;
        }
        join_res.have_hub_static = 1;
        if (store_save_hub_static(join_res.hub_static) == 0)
            out("hub static key set and persisted\r\n");
        else
            out("hub static key set for this session only - flash refused it, "
                "so a reset loses it\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "show") == 0) {
        uint8_t fp[SHA256_LEN];
        out("dev %08lX  hub key %s\r\n", (unsigned long)pair_ctx.dev_id,
            join_hub_static_ready(&join_res) ? "set" : "NOT SET");
        if (pairing_fingerprint(&pair_ctx, fp, sizeof(fp))) {
            out("fingerprint ");
            for (int i = 0; i < SHA256_LEN; i++) out("%02X", fp[i]);
            out("\r\n");
        }
        if (join_res.paired)
            paired_line();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "invited") == 0) {
        /* The v3 flow: the exchange is byte for byte v2, but a forged frame cannot start it.
         * radio_devices_docs/wl55_device/radio/pairing.md */
        uint32_t ms = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 30000u;
        pair_init_ctx_t c = {0};
        store_state_t st;
        if (!pair_init_live_ctx(&c, &st)) {
            out("no identity or hub key yet\r\n");
            return 0;
        }
        int rc = join_run_invited(&pair_ctx, &join_res, ms, &c, st.init_ceiling);
        if (rc == 0) {
            paired_line();
        } else if (rc == -30) {
            join_stats_t s2;
            join_stats(&s2);
            out("invitation refused: %s\r\n",
                pair_init_rc_name((pair_init_rc_t)s2.invite_refused));
        } else {
            out("not paired (%d) - `join stats` names the stage\r\n", rc);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        uint32_t ms = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 5000u;
        int rc = join_run(&pair_ctx, &join_res, ms);
        if (rc == 0)
            paired_line();
        else
            out("not paired (%d) - `join stats` names the stage\r\n", rc);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "probe") == 0) {
        uint32_t len = strtoul(argv[2], NULL, 10);
        uint32_t ms = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 5000u;
        uint32_t air = 0, delay = 0;
        int rc = join_probe((uint8_t)len, ms, &air, &delay);
        if (rc == 0)
            out("probe %lu bytes sent, %lu us after the beacon, %lu us keyed\r\n",
                (unsigned long)len, (unsigned long)delay, (unsigned long)air);
        else
            out("probe not sent (%d) - `join stats` names the beacon stage\r\n", rc);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "frame") == 0) {
        uint8_t built[64], chip[64];
        uint8_t n = join_last_request(built, sizeof(built));
        if (n == 0 || built[0] == 0) {
            out("no request built yet\r\n");
            return 0;
        }
        out("built:");
        for (uint8_t i = 0; i < n; i++) out(" %02X", built[i]);
        out("\r\n");
        /* The chip's own copy, so the SPI write is inside the comparison.
         * radio_devices_docs/wl55_device/testing/console.md */
        if (radio_read_tx_buffer(chip, n) == 0) {
            out("chip :");
            for (uint8_t i = 0; i < n; i++) out(" %02X", chip[i]);
            out("\r\n%s\r\n", memcmp(built, chip, n) == 0 ? "identical"
                                                            : "DIFFER - the SPI write");
        } else {
            out("chip : read failed\r\n");
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        join_selftest_t r;
        join_selftest(&r);
        out("accept %s (rc %d)  grant %s  hop-key %s\r\n",
            r.accept_ok ? "ok" : "FAIL", r.accept_rc, r.grant_ok ? "ok" : "FAIL",
            r.hop_key_ok ? "ok" : "FAIL");
        out("forged %s  outside-window %s  confirm %s\r\n",
            r.forged_rejected ? "rejected" : "FAIL",
            r.stale_rejected ? "rejected" : "FAIL",
            r.confirm_ok ? "ok" : "FAIL");
        out("nonce ");
        for (int i = 0; i < 12; i++) out("%02x", r.nonce[i]);
        out("  aad %u  ct %u\r\n", r.aad_len, r.ct_len);
        out("req-built %s  rsp-parsed %s  conf-built %s  eph-static %s\r\n",
            r.req_built_ok ? "ok" : "FAIL", r.rsp_parsed_ok ? "ok" : "FAIL",
            r.conf_built_ok == 1u ? "ok" : (r.conf_built_ok == 2u ? "skipped" : "FAIL"),
            r.eph_static_rejected ? "rejected" : "FAIL");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
        join_stats_t s;
        join_stats(&s);
        out("beacon: silent %lu  crc-err %lu  not-a-beacon %lu  "
            "window-closed %lu\r\n",
            (unsigned long)s.beacon_timeout, (unsigned long)s.beacon_crc_err,
            (unsigned long)s.beacon_other, (unsigned long)s.beacon_closed);
        out("last frame heard: %u bytes  type %02X  %d dBm\r\n",
            (unsigned)s.last_len, (unsigned)s.last_type, (int)s.last_rssi_dbm);
        out("beacon -> request on air: %lu us  (hub retunes ~100000 us "
            "after its beacon)\r\n", (unsigned long)s.beacon_to_req_us);
        out("req sent %lu   rsp: timeout %lu  crc-err %lu  type-04 %lu  "
            "skipped %lu (last %02X)\r\n",
            (unsigned long)s.req_sent, (unsigned long)s.rsp_timeout,
            (unsigned long)s.rsp_crc_err, (unsigned long)s.rsp_heard,
            (unsigned long)s.rsp_skipped, s.rsp_other_type);
        out("last response: %u bytes  type %02X  version %02X  %d dBm"
            "   (want %u/%02X/%02X)\r\n",
            (unsigned)s.rsp_len, (unsigned)s.rsp_type, (unsigned)s.rsp_version,
            (int)s.rsp_rssi_dbm, (unsigned)sizeof(radio_pair_rsp_t),
            (unsigned)RADIO_FRAME_PAIR_RSP, (unsigned)RADIO_PROTO_VERSION);
        out("rsp refused: frame %lu  ids %lu  point %lu  eph-static %lu  "
            "confirm %lu\r\n",
            (unsigned long)s.rsp_bad_frame, (unsigned long)s.rsp_wrong_ids,
            (unsigned long)s.rsp_bad_point, (unsigned long)s.rsp_eph_is_static,
            (unsigned long)s.rsp_confirm_bad);
        out("conf sent %lu   accept: timeout %lu  crc-err %lu  heard %lu  "
            "skipped %lu (last %02X)\r\n",
            (unsigned long)s.conf_sent, (unsigned long)s.accept_timeout,
            (unsigned long)s.accept_crc_err, (unsigned long)s.accept_heard,
            (unsigned long)s.accept_skipped, s.accept_other_type);
        out("store failed %lu   (paired on air but not on flash)\r\n",
            (unsigned long)s.store_failed);
        out("accept refused: frame %lu  window %lu  tag %lu   paired %lu\r\n",
            (unsigned long)s.accept_bad_frame,
            (unsigned long)s.accept_outside_window,
            (unsigned long)s.accept_bad_tag, (unsigned long)s.paired);
        /* The number the hub cannot infer: refused and never received read the same there.
         * radio_devices_docs/wl55_device/testing/console.md */
        out("heard-but-refused is rsp_heard minus what follows it\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        join_stats_reset();
        out("counters cleared\r\n");
        return 0;
    }
    out("join hub-key <hex>  - the hub's static public key, 66 hex digits\r\n");
    out("join show      - dev id, fingerprint, whether the hub key is set\r\n");
    out("join run [ms]  - the four-frame exchange, started by a beacon (v2)\r\n");
    out("join invited [ms] - the same exchange, started by a MACed invitation (v3)\r\n");
    out("join probe <len> [ms] - one frame of <len> bytes, same rendezvous\r\n");
    out("join selftest  - the whole exchange against the published frames\r\n");
    out("join stats     - per-stage counters; refusal names itself\r\n");
    out("join reset     - clear the counters\r\n");
    return 0;
}

static hop_ctx_t hopper;

/* Chosen in one place: two branches had already disagreed about which key.
 * radio_devices_docs/wl55_device/testing/console.md */
static const char *hop_live_key(void) {
    return join_res.paired ? "network" : "vector";
}

static int hop_init_live(void) {
    return hop_init(&hopper, join_res.paired ? join_res.hop_key : vec_hop_key,
                    HOP_VECTORS_COUNT);
}

static int join_is_paired(void) { return join_res.paired != 0u; }

static int hop_channel_live(uint32_t sf, uint8_t *ch) {
    return (hop_init_live() != 0) ? -1 : hop_channel(&hopper, sf, ch);
}

/* Reports which convention the other end used rather than leaving it guessed.
 * radio_devices_docs/wl55_device/testing/console.md */
static int cmd_hop(int argc, char **argv) {
    uint8_t ch, grid;

    if (hop_init(&hopper, vec_hop_key, HOP_VECTORS_COUNT) != 0) {
        out("hop init failed\r\n");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "vec") == 0) {
        int deck_ok = 1, sample_ok = 1, grid_ok = 1, disjoint_ok = 1;
        uint32_t t0 = micros();

        for (uint32_t sf = 0; sf < HOP_VECTORS_COUNT; sf++) {
            if (hop_channel(&hopper, sf, &ch) != 0 || ch != vec_hop_deck_cycle0[sf])
                deck_ok = 0;
            if (hop_channel(&hopper, sf + HOP_VECTORS_COUNT, &ch) != 0 ||
                ch != vec_hop_deck_cycle1[sf])
                deck_ok = 0;
        }
        for (int i = 0; i < HOP_VECTORS_SAMPLES; i++) {
            if (hop_channel(&hopper, vec_hop_superframe[i], &ch) != 0 ||
                ch != vec_hop_channel[i]) { sample_ok = 0; continue; }
            grid = hop_to_grid(ch);
            if (grid != vec_hop_grid_slot[i] || radio_slot_hz(grid) != vec_hop_hz[i])
                grid_ok = 0;
        }
        for (uint8_t i = 0; i < HOP_VECTORS_COUNT; i++)
            if (hop_to_grid(i) == HOP_VECTORS_JOIN || hop_to_grid(i) >= HOP_VECTORS_GRID)
                disjoint_ok = 0;
        uint32_t us = micros() - t0;

        out("deck %s  samples %s  grid %s  join disjoint %s\r\n",
            deck_ok ? "ok" : "FAIL", sample_ok ? "ok" : "FAIL",
            grid_ok ? "ok" : "FAIL", disjoint_ok ? "ok" : "FAIL");
        out("vectors %s, %lu us\r\n", HOP_VECTORS_DIGEST, (unsigned long)us);

        /* Shown on this board's accelerator: a 32-bit datatype silently changes the PRF.
         * radio_devices_docs/wl55_device/testing/console.md */
        uint8_t block[16] = {0}, a8[16], a32[16], model[16];
        block[3] = 1;
        if (crypto_aes_ecb_datatype_probe(vec_hop_key, block, a8, a32) != 0) {
            out("datatype probe failed\r\n");
            return 0;
        }
        hop_swap32_model(vec_hop_key, block, model);
        out("datatype 32b differs from 8b: %s, and matches the word-swap model: %s\r\n",
            memcmp(a8, a32, sizeof(a8)) ? "yes" : "no",
            memcmp(model, a32, sizeof(a32)) ? "NO" : "yes");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "id") == 0) {
        if (argc < 4) {
            out("usage: hop id <superframe> <channel>\r\n");
            return 0;
        }
        uint32_t sf = (uint32_t)strtoul(argv[2], NULL, 10);
        uint8_t observed = (uint8_t)atoi(argv[3]);
        if (hop_init_live() != 0) {
            out("hop init failed\r\n");
            return 0;
        }
        const char *name = hop_identify(&hopper, sf, observed);
        if (name == NULL)
            out("channel %u at superframe %lu matches no known convention "
                "[%s key] - wrong key, wrong counter or a different hop set\r\n",
                (unsigned)observed, (unsigned long)sf, hop_live_key());
        else
            out("channel %u at superframe %lu is the %s convention [%s key]\r\n",
                (unsigned)observed, (unsigned long)sf, name, hop_live_key());
        return 0;
    }

    uint32_t sf = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 10)
                              : superframe_now(&sframe);
    /* Named on every line: a channel from the wrong key is a plausible number.
     * radio_devices_docs/wl55_device/testing/console.md */
    const char *key_name = hop_live_key();
    if (hop_init_live() != 0 || hop_channel(&hopper, sf, &ch) != 0) {
        out("prf failed\r\n");
        return 0;
    }
    grid = hop_to_grid(ch);
    out("superframe %lu -> hop %u -> grid %u -> %lu Hz  [%s key]\r\n",
        (unsigned long)sf, (unsigned)ch, (unsigned)grid,
        (unsigned long)radio_slot_hz(grid), key_name);
    return 0;
}

/* Idle is what is left over, so it is a lower bound on load and not an upper one.
 * radio_devices_docs/wl55_device/testing/console.md */
static int cmd_load(int argc, char **argv) {
    uint32_t window, busy = 0, recoverable = 0;

    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        load_reset();
        out("window restarted\r\n");
        return 0;
    }

    window = load_window_us();
    if (window == 0) {
        out("no window - run 'load reset' first\r\n");
        return 0;
    }

    out("window %lu us\r\n", (unsigned long)window);
    for (int c = 0; c < LOAD_CATEGORIES; c++) {
        uint32_t us = load_us((load_cat_t)c);
        uint32_t n = load_calls((load_cat_t)c);
        busy += us;
        if (c == LOAD_PKA || c == LOAD_RADIO_WAIT)
            recoverable += us;
        out("%-11s %8lu us %6lu.%02lu%%  %6lu calls  max %7lu us\r\n",
            load_name((load_cat_t)c), (unsigned long)us,
            (unsigned long)(100ul * us / window),
            (unsigned long)((10000ull * us / window) % 100ul),
            (unsigned long)n, (unsigned long)load_max_us((load_cat_t)c));
    }
    out("busy %lu.%02lu%%, of which recoverable %lu.%02lu%%, idle %lu.%02lu%%\r\n",
        (unsigned long)(100ul * busy / window),
        (unsigned long)((10000ull * busy / window) % 100ul),
        (unsigned long)(100ul * recoverable / window),
        (unsigned long)((10000ull * recoverable / window) % 100ul),
        (unsigned long)(100ul * (window - busy) / window),
        (unsigned long)((10000ull * (window - busy) / window) % 100ul));
    return 0;
}

#include "pair_v2.h"   /* the published uplink frame, for the assembly check */
#include "link_v5.h"   /* both data frames at 39 bytes, and the only downlink vector */

/* A size assert answers "same shape", never "same contract": v4 built clean on v5.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
_Static_assert(LINK_VECTORS_VERSION == RADIO_LINK_VERSION,
               "the link vectors are not the wire this build speaks");

/* The grid is in hub microseconds; this clock is fast and lands early.
 * radio_devices_docs/wl55_device/radio/timebase.md */
static uint32_t hub_us_to_local(uint32_t hub_us);

/* What the next report echoes. The wire cannot tell its zero from cmd 0 seq 0. */
static uint8_t  dl_ack_seq, dl_ack_cmd, dl_ack_arg, dl_any;
static uint32_t dl_applied, dl_repeats, dl_replays;

/* A silence this device imposed on itself. A reboot is not one: uptime_s says that.
 * radio_devices_docs/radio/tdma.md */
static uint8_t tx_self_silenced;

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
static int cmd_uplink(int argc, char **argv) {
    radio_uplink_report_t rep;

    /* The controls, shipped with the instrument, not after it disappoints. */
    if (argc >= 3 && strcmp(argv[1], "selftest") == 0 &&
        strcmp(argv[2], "mutate") == 0) {
        uint8_t v[sizeof(radio_uplink_t)];
        uint8_t d[sizeof(radio_downlink_t)];
        radio_uplink_report_t vr = { -92, 0x05, 3287, 61, 0x5b, 0x01, 0x1f, 0x04,
                                     {0xa1, 0xb2, 0xc3, 0xd4} };
        radio_downlink_cmd_t dc;
        int slot_seen, hdr_seen;

        /* Slot 65 instead of 66: it must reach the header and the nonce both. */
        slot_seen = uplink_build(0x1a2b3c58u, 65u, 0x0000002au, LV_KEY_SESSION,
                                 &vr, v) == 0 &&
                    memcmp(v, LV_FRAME_UPLINK, sizeof(v)) != 0;

        /* The version byte is inside the AAD, so the open must refuse it. */
        memcpy(d, LV_FRAME_DOWNLINK, sizeof(d));
        d[1] = (uint8_t)(d[1] ^ 0x10u);
        hdr_seen = downlink_open_with(d, (uint8_t)sizeof(d), 1u, LV_KEY_SESSION,
                                      0x0000002au, &dc) != 0;

        out("slot 65 changes the frame:       %s\r\n", slot_seen ? "ok" : "VACUOUS");
        out("a flipped header byte refuses:   %s\r\n", hdr_seen ? "ok" : "VACUOUS");
        return 0;
    }
    /* link_v4's slot is 66, so the slot byte is covered in header and nonce. */
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        uint8_t v[sizeof(radio_uplink_t)];
        uint8_t d[sizeof(radio_downlink_t)];
        radio_uplink_report_t vr = { -92, 0x05, 3287, 61, 0x5b, 0x01, 0x1f, 0x04,
                                     {0xa1, 0xb2, 0xc3, 0xd4} };
        radio_downlink_cmd_t dc;
        int up, down;

        up = uplink_build(0x1a2b3c58u, 66u, 0x0000002au, LV_KEY_SESSION,
                          &vr, v) == 0 &&
             memcmp(v, LV_FRAME_UPLINK, sizeof(v)) == 0;
        memcpy(d, LV_FRAME_DOWNLINK, sizeof(d));
        down = downlink_open_with(d, (uint8_t)sizeof(d), 1u, LV_KEY_SESSION,
                                  0x0000002au, &dc) == 0 &&
               memcmp(&dc, LV_DOWNLINK_PLAIN, sizeof(dc)) == 0;

        out("link_v%u %s\r\n", LINK_VECTORS_VERSION, LINK_VECTORS_DIGEST);
        out("uplink assembly vs LV_FRAME_UPLINK:  %s\r\n", up ? "ok" : "FAIL");
        out("downlink open vs LV_FRAME_DOWNLINK:  %s\r\n", down ? "ok" : "FAIL");
        out("covers type, version, slot 66, superframe, nonce, AAD, 16-byte body\r\n");
        out("run `uplink selftest mutate` - neither check is readable until it fails\r\n");
        return 0;
    }
    /* The refusal's control: re-seal under a claimed nonce. Sends nothing. */
    if (argc >= 2 && strcmp(argv[1], "again") == 0) {
        uint8_t v[sizeof(radio_uplink_t)];
        radio_uplink_report_t vr = {0, 0, 0, 0};
        int rc;

        if (!seal_any) {
            out("nothing claimed yet - send one uplink first\r\n");
            return 0;
        }
        rc = uplink_seal(seal_last_sf, join_res.slot, join_res.dev_id_be,
                         join_res.session, &vr, v);
        out("re-seal under superframe %lu slot %u: %s  (refusals %lu)\r\n",
            (unsigned long)seal_last_sf, (unsigned)join_res.slot,
            rc == -2 ? "refused" : "ACCEPTED - the guard is vacuous",
            (unsigned long)seal_refused);
        return 0;
    }
    /* Kept across calls so `replay` re-sends the bytes that were actually sealed.
     * radio_devices_docs/wl55_device/testing/console.md */
    static uint8_t last_f[sizeof(radio_uplink_t)];
    static uint32_t last_sf;
    static uint8_t have_last;
    int replay = (argc >= 2 && strcmp(argv[1], "replay") == 0);

    uint8_t f[sizeof(radio_uplink_t)];
    uint8_t hop, grid;

    if (replay && !have_last) {
        out("nothing to replay - send one uplink first\r\n");
        return 0;
    }
    if (!join_is_paired()) {
        out("not paired - nothing has granted a slot\r\n");
        return 0;
    }
    if (!superframe_can_schedule(&sframe)) {
        /* Not `state`, which reports the last beacon ever heard rather than a fresh one.
         * radio_devices_docs/wl55_device/testing/console.md */
        out("refusing: counter is %s, %lu ms since a beacon, period %s\r\n",
            superframe_state_name(&sframe),
            (unsigned long)(superframe_since_beacon_us(&sframe) / 1000u),
            sframe.measured_us ? "measured" : "STUB (deliberately wrong)");
        out("a wrong counter is a wrong nonce as well as a wrong channel - "
            "run \"time follow\" twice to measure the period\r\n");
        return 0;
    }

    uint32_t sf = superframe_now(&sframe) + 1u;
    uint32_t boundary = sframe.next_boundary_us;
    uint32_t slot_at  = boundary + hub_us_to_local(RADIO_SLOT_OFFSET_US(join_res.slot));

    /* The live channel: a stale one would test tuning rather than the replay floor.
     * radio_devices_docs/wl55_device/testing/console.md */
    if (hop_channel_live(sf, &hop) != 0) {
        out("hop failed\r\n");
        return 0;
    }
    grid = hop_to_grid(hop);
    if (radio_configure(grid) != 0) {
        out("configure failed\r\n");
        return 0;
    }

    /* Zeroed first: an app_len off the stack is a length the hub would trust. */
    memset(&rep, 0, sizeof(rep));
    rep.rssi_down = beacon_rssi_valid ? beacon_rssi_dbm : 0;
    /* Flagged rather than replaced: a sentinel cannot be told from a real -0 dBm.
     * radio_devices_docs/wl55_device/testing/console.md */
    rep.flags     = RADIO_REPORT_FLAG_SUPPLY_STALE;
    if (!beacon_rssi_valid)
        rep.flags |= RADIO_REPORT_FLAG_RSSI_STALE;
    rep.supply_mv = 0;              /* no ADC on this build; 0 is not a reading */
    rep.uptime_s  = timebase_uptime_s();
    rep.ack_seq   = dl_ack_seq;
    rep.ack_cmd   = dl_ack_cmd;
    rep.ack_arg   = dl_ack_arg;
    if (tx_self_silenced)
        rep.flags |= RADIO_REPORT_FLAG_RESUMED;

    /* An unreserved counter is nonce reuse after the next reboot. */
    if (!replay && !tx_gate(sf)) {
        tx_self_silenced = 1;
        return 0;
    }

    if (replay) {
        /* Re-sending sealed bytes claims nothing: the nonce was spent already. */
        memcpy(f, last_f, sizeof(f));
        sf = last_sf;
    } else {
        int src = uplink_seal(sf, join_res.slot, join_res.dev_id_be,
                              join_res.session, &rep, f);
        if (src == -2) {
            out("refusing: superframe %lu slot %u is already sealed under, and a "
                "second plaintext under one nonce loses the GCM subkey\r\n",
                (unsigned long)sf, (unsigned)join_res.slot);
            return 0;
        }
        if (src != 0) {
            out("seal failed\r\n");
            return 0;
        }
    }

    uint32_t fire = slot_at - UPLINK_LEAD_US;
    while (!timebase_elapsed(fire)) { }

    uint32_t air = 0;
    int rc = radio_send(f, sizeof(f), &air);
    /* `air` is SetTx to TxDone, so subtracting the frame measures this ramp.
     * radio_devices_docs/wl55_device/testing/console.md */
    uint32_t on_air_us = radio_tx_air_time_us((uint8_t)sizeof(f));
    uint32_t ramp = (air > on_air_us) ? (air - on_air_us) : 0u;
    /* Signed, both instants: early is as wrong as late, and the grid wants the first bit.
     * radio_devices_docs/wl55_device/testing/console.md */
    int32_t set_err   = (int32_t)(micros() - air - slot_at);
    int32_t first_bit = set_err + (int32_t)ramp;
    if (rc != 0) {
        out("tx failed (%d)\r\n", rc);
        return 0;
    }
    if (!replay) {
        /* Spent on the frame that carried it; replayed bytes carry the old flags. */
        tx_self_silenced = 0;
        memcpy(last_f, f, sizeof(f));
        last_sf = sf;
        have_last = 1;
    }
    out("%suplink %u bytes, slot %u, superframe %lu, grid %u (%lu Hz)\r\n",
        replay ? "REPLAYED " : "",
        (unsigned)sizeof(f), (unsigned)join_res.slot, (unsigned long)sf,
        (unsigned)grid, (unsigned long)radio_slot_hz(grid));
    /* The preamble is the air term's denominator and nothing else logs it. */
    out("keyed %lu us = %lu air + %lu ramp (measured) at %u B preamble\r\n",
        (unsigned long)air, (unsigned long)on_air_us, (unsigned long)ramp,
        (unsigned)radio_preamble_bytes());
    out("SetTx %+ld us from the slot boundary, first bit %+ld us\r\n",
        (long)set_err, (long)first_bit);
    /* All the slack is late: air 17600 + guard 1400, early none. On slot 0 early is downlink.
     * radio_devices_docs/wl55_device/testing/console.md */
    static uint32_t out_early, out_late;
    if (first_bit < 0)                                  out_early++;
    else if (first_bit > (int32_t)RADIO_SLOT_GUARD_US)  out_late++;
    out("slot margin: early 0 us, late %u us  (%lu early, %lu late so far)\r\n",
        (unsigned)RADIO_SLOT_GUARD_US, (unsigned long)out_early,
        (unsigned long)out_late);
    if (replay)
        out("same sealed bytes as superframe %lu - a valid tag the hub has "
            "already accepted\r\n", (unsigned long)last_sf);
    else
        out("report: rssi_down %d dBm  flags %02X  supply %u mV  uptime %lu s"
            "  ack %u/%u arg %u\r\n",
            (int)rep.rssi_down, rep.flags, rep.supply_mv,
            (unsigned long)rep.uptime_s, rep.ack_cmd, rep.ack_seq, rep.ack_arg);
    return 0;
}


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
    if (cmd->cmd == RADIO_CMD_SET_RATE && cmd->report_every != 0u) {
        join_res.report_every = cmd->report_every;
        rate_unsaved = 1u;
    }
    dl_ack_seq = cmd->cmd_seq;
    dl_ack_cmd = cmd->cmd;
    /* What was applied, not what was asked: only SET_RATE carries an argument. */
    dl_ack_arg = (cmd->cmd == RADIO_CMD_SET_RATE) ? join_res.report_every : 0u;
    dl_any     = 1u;
    dl_applied++;
    tlm_emit(TLM_RX_CMD, sf, cmd->cmd, cmd->cmd_seq, 0u);
}


/* Reports bytes and refuses to interpret them: the window and rate are contract.
 * radio_devices_docs/wl55_device/testing/console.md */
static int cmd_downlink(int argc, char **argv) {
    uint8_t rx[64];
    radio_rx_info_t info = {0};
    uint8_t hop, grid;

    /* Accepted bytes, refused now: only the floor can be the reason.
     * radio_devices_docs/wl55_device/security/replay.md */
    if (argc >= 2 && strcmp(argv[1], "replay") == 0) {
        radio_downlink_cmd_t cmd;
        int rc;
        if (!have_last_dl) {
            out("nothing to replay - accept one downlink first\r\n");
            return 0;
        }
        rc = downlink_open(last_dl, (uint8_t)sizeof(last_dl), &cmd);
        out("replayed the frame accepted at %lu: %s (%d)\r\n",
            (unsigned long)dl_floor, (rc == -4) ? "refused" : "ACCEPTED - no floor",
            rc);
        return 0;
    }

    int wide = (argc >= 2 && strcmp(argv[1], "wide") == 0);
    uint32_t frames = (argc >= (wide ? 3 : 2))
                          ? (uint32_t)strtoul(argv[wide ? 2 : 1], NULL, 10) : 4u;

    /* Re-aligns rather than loosening the gate; a wrong channel guess costs one window.
     * radio_devices_docs/wl55_device/testing/console.md */
    if (!superframe_can_schedule(&sframe) && sframe.aligned) {
        uint8_t hop0, grid0;
        uint32_t sf0 = superframe_now(&sframe) + 1u;
        if (hop_channel_live(sf0, &hop0) == 0 &&
            radio_configure((grid0 = hop_to_grid(hop0))) == 0) {
            info.timeout_us = 2u * SUPERFRAME_US;
            if (radio_receive(rx, sizeof(rx), &info) == 0) {
                uint32_t got = 0;
                if (beacon_apply(rx, info.len, &sframe, &quiesce, info.start_us, &got) == BEACON_OK) {
                    beacon_rssi_dbm = info.rssi_dbm;
                    beacon_rssi_valid = 1;
                    out("re-aligned to %lu on grid %u  rssi %d dBm\r\n",
                        (unsigned long)got, (unsigned)grid0, info.rssi_dbm);
                }
            }
        }
    }
    if (!superframe_can_schedule(&sframe)) {
        out("refusing: counter is %s, %lu ms since a beacon, period %s\r\n",
            superframe_state_name(&sframe),
            (unsigned long)(superframe_since_beacon_us(&sframe) / 1000u),
            sframe.measured_us ? "measured" : "STUB (deliberately wrong)");
        return 0;
    }
    uint32_t frames_asked = frames;
    if (frames == 0u || frames > 32u) frames = 4u;
    /* The count was clamped to the no-argument default and never printed at all.
     * radio_devices_docs/wl55_device/testing/console.md */
    out("listening for %lu superframes%s\r\n", (unsigned long)frames,
        (frames_asked != frames) ? " (1..32; yours refused)" : "");

    /* Where the frames are, measured against my boundary, not whether they are expected.
     * radio_devices_docs/wl55_device/testing/console.md */
    if (wide) {
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t sf = superframe_now(&sframe) + 1u;
            uint32_t boundary = sframe.next_boundary_us;
            uint8_t h, g;
            if (hop_channel_live(sf, &h) != 0 ||
                radio_configure((g = hop_to_grid(h))) != 0) {
                out("hop/configure failed\r\n");
                return 0;
            }
            while (!timebase_elapsed(boundary)) { }
            out("sf %lu grid %u%s:\r\n", (unsigned long)sf, (unsigned)g,
                RADIO_DOWNLINK_ON(sf) ? " (downlink superframe)" : "");
            uint32_t deadline = boundary + SUPERFRAME_US - 50000u;
            while (!timebase_elapsed(deadline)) {
                uint32_t left = deadline - micros();
                if ((int32_t)left <= 0) break;
                info.timeout_us = left;
                if (radio_receive(rx, sizeof(rx), &info) != 0)
                    break;
                uint32_t at = micros() - boundary
                              - radio_rx_air_time_us((uint8_t)info.len);
                out("  +%lu us: %u bytes type %02X rssi %d dBm\r\n",
                    (unsigned long)at, info.len, info.len ? rx[0] : 0u,
                    info.rssi_dbm);
            }
        }
        return 0;
    }

    for (uint32_t i = 0; i < frames; i++) {
        uint32_t sf = superframe_now(&sframe) + 1u;
        uint32_t boundary = sframe.next_boundary_us;
        if (!RADIO_DOWNLINK_ON(sf)) {
            /* Printed, not skipped: wrong parity and a silent hub look identical.
             * radio_devices_docs/wl55_device/testing/console.md */
            out("sf %lu: no downlink this superframe (parity)\r\n",
                (unsigned long)sf);
            while (!timebase_elapsed(boundary + 1000u)) { }
            continue;
        }
        if (hop_channel_live(sf, &hop) != 0 ||
            radio_configure((grid = hop_to_grid(hop))) != 0) {
            out("hop/configure failed\r\n");
            return 0;
        }
        /* Open early, whole region: the frame is at +25 059 and a receiver at +25 000 hears none.
         * radio_devices_docs/wl55_device/testing/console.md */
        while (!timebase_elapsed(boundary + RADIO_DOWNLINK_RX_OPEN_US
                                 - DOWNLINK_RX_LEAD_US)) { }
        info.timeout_us = RADIO_DOWNLINK_RX_CLOSE_US - RADIO_DOWNLINK_RX_OPEN_US
                          + DOWNLINK_RX_LEAD_US;
        int rc = radio_receive(rx, sizeof(rx), &info);
        if (rc == 0) {
            radio_downlink_cmd_t cmd;
            int orc = downlink_open(rx, info.len, &cmd);
            out("sf %lu grid %u: %u bytes, rssi %d dBm  ",
                (unsigned long)sf, (unsigned)grid, info.len, info.rssi_dbm);
            for (uint8_t j = 0; j < info.len && j < 24u; j++) out("%02X ", rx[j]);
            out("\r\n");
            if (orc == 0) {
                downlink_apply(sf, &cmd);
                /* An unknown cmd is a keepalive: refusing it stops the one this device understands.
                 * radio_devices_docs/wl55_device/testing/console.md */
                out("  opened: cmd %u%s  report_every %u  arg %u  hub_time %lu s\r\n",
                    cmd.cmd, cmd.cmd > RADIO_CMD_REJOIN ? " (unknown, ignored)" : "",
                    cmd.report_every, cmd.arg, (unsigned long)cmd.hub_time_s);
                out("  seq %u  app_len %u  -> ack %u/%u arg %u\r\n", cmd.cmd_seq,
                    cmd.app_len, dl_ack_cmd, dl_ack_seq, dl_ack_arg);
            } else if (orc == -2)
                out("  addressed to slot %u, not mine (%u)\r\n",
                    rx[2], (unsigned)join_res.slot);
            else if (orc == -3)
                out("  TAG FAILED - wrong key, slot or superframe\r\n");
            else if (orc == -4)
                out("  REPLAY - a valid tag at or below the floor %lu\r\n",
                    (unsigned long)dl_floor);
            else
                out("  not a downlink frame: type %02X version %02X len %u\r\n",
                    rx[0], info.len > 1u ? rx[1] : 0u, info.len);
        } else if (rc == -3) {
            out("sf %lu grid %u: a frame arrived and failed CRC\r\n",
                (unsigned long)sf, (unsigned)grid);
        } else {
            out("sf %lu grid %u: region empty (%lu us open)\r\n",
                (unsigned long)sf, (unsigned)grid,
                (unsigned long)info.timeout_us);
        }
    }
    /* Counted since boot by both paths, and until now by neither reader. */
    out("downlink: %lu applied, %lu repeats, %lu replays, floor %s%lu\r\n",
        (unsigned long)dl_applied, (unsigned long)dl_repeats,
        (unsigned long)dl_replays, dl_floor_known ? "" : "unset ",
        (unsigned long)dl_floor);
    return 0;
}

/* The granted cadence, disarmed after a reset.
 * radio_devices_docs/wl55_device/radio/pairing.md */
#define REPORT_BOUNDARY_LEAD_US  100000u
/* Caps the blocking receive; past that only a re-camp helps. */
#define REPORT_UNCERT_MAX_US     120000u

static uint8_t  report_armed;
/* Which of the k opportunities the loop uses. radio_devices_docs/radio/tdma.md */
static uint8_t  report_opp;
/* Use every k rather than one, for repeats on a single channel. */
static uint8_t  report_opp_all;
/* 0 any, 1 below the join channel, 2 above: makes a counting receiver
 * discriminate. radio_devices_docs/radio/hopping.md */
static uint8_t  report_band;
static uint32_t report_attempt_sf;

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

static uint8_t  recover_armed = 1u;
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

static const char *recover_state_name(void) {
    switch (recover_state) {
    case RECOVER_SEARCH: return "search";
    case RECOVER_PARK:   return "park";
    default:             return "idle";
    }
}

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
    tlm_emit(TLM_REC_HIT, aligned, grid, (uint32_t)(int32_t)info->rssi_dbm, 0u);
    if (aligned != before)
        tlm_emit(TLM_SYNC_JUMP, aligned, before, (uint32_t)(int32_t)(aligned - before), 0u);
    /* Predicting the next beacon beats holding a channel another cycle. */
    if (recover_state == RECOVER_PARK && sframe.running && sframe.aligned) {
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

    uint32_t boundary = sframe.next_boundary_us;
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
        recover_park_grid = hop_to_grid((uint8_t)(r % HOP_VECTORS_COUNT));
        tlm_emit(TLM_REC_PARK, sframe.counter, recover_park_grid,
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
    if (!recover_armed || !join_is_paired())
        return;
    /* Nothing typed starts the clock on this path. */
    time_start();

    /* One beacon aligns and leaves the stub period: aligned and unusable. */
    if (!recover_park_forced && superframe_can_schedule(&sframe)) {
        if (recover_state != RECOVER_IDLE)
            tlm_emit(TLM_REC_EXIT, sframe.counter, sframe.measured_us, 0u, 0u);
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
        tlm_emit(TLM_REC_ENTER, sframe.counter,
                 (sframe.measured_us != 0u && sframe.running) ? 1u : 2u, 0u, 0u);
        /* No measured period is nothing to extrapolate from. */
        if (sframe.measured_us != 0u && sframe.running) {
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

static int cmd_recover(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        recover_armed = 1;
        out("armed\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        recover_armed = 0;
        recover_state = RECOVER_IDLE;
        recover_park_forced = 0;
        out("disarmed\r\n");
        return 0;
    }
    /* On demand: the state it recovers from is rare, and so is the test. */
    if (argc >= 2 && strcmp(argv[1], "park") == 0) {
        if (!join_is_paired()) {
            out("not paired - no hop key to test a channel against\r\n");
            return 0;
        }
        recover_armed = 1;
        recover_park_forced = 1;
        recover_state = RECOVER_PARK;
        recover_park_grid = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 0xFFu;
        out("parking on %s\r\n",
            (argc >= 3) ? "the given grid slot" : "a drawn grid slot");
        return 0;
    }

    out("%s  %s  entered %lu\r\n", recover_armed ? "armed" : "disarmed",
        recover_state_name(), (unsigned long)recover_entered);
    out("search: %lu windows, %lu aligned, %lu since the last hit\r\n",
        (unsigned long)recover_search_windows, (unsigned long)recover_search_hits,
        (unsigned long)recover_search_tries);
    if (recover_state == RECOVER_PARK && recover_park_grid != 0xFFu)
        out("park: grid %u (%lu Hz)  %lu chunks, %lu aligned\r\n",
            (unsigned)recover_park_grid,
            (unsigned long)radio_slot_hz(recover_park_grid),
            (unsigned long)recover_park_chunks, (unsigned long)recover_park_hits);
    else
        out("park: %lu chunks, %lu aligned\r\n",
            (unsigned long)recover_park_chunks, (unsigned long)recover_park_hits);
    /* A zero after a forged beacon means unwired, not passed. */
    out("refused: %lu counter/channel, %lu beacon (%s)\r\n",
        (unsigned long)recover_mismatch, (unsigned long)recover_rejected,
        beacon_rc_name(recover_last_rc));
    if (recover_mismatch)
        out("last mismatch: superframe %lu on grid %u\r\n",
            (unsigned long)recover_mismatch_sf, (unsigned)recover_mismatch_grid);
    if (recover_park_sf)
        out("last accepted: superframe %lu\r\n", (unsigned long)recover_park_sf);
    return 0;
}

void report_service(void) {
    uint8_t rx[32], f[sizeof(radio_uplink_t)], hop, grid;
    radio_rx_info_t info = {0};
    radio_uplink_report_t rep;
    uint32_t aligned = 0;

    if (!report_armed || !join_is_paired() || sframe.measured_us == 0u)
        return;
    /* Two services retuning leaves neither one's window open. */
    if (recover_state != RECOVER_IDLE)
        return;

    uint32_t sf = superframe_now(&sframe) + 1u;
    /* Anchored to the hub's counter: counting from the last send re-phases.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    if (join_res.report_every == 0u || (sf % join_res.report_every) != 0u)
        return;
    if (report_attempt_sf == sf)
        return;

    /* Sized by staleness: a fixed window makes each miss likelier. */
    uint32_t stale = micros() - sframe.last_beacon_us;
    uint32_t uncert = stale / 1000u;               /* 1000 ppm of the free run */
    if (uncert > REPORT_UNCERT_MAX_US)
        uncert = REPORT_UNCERT_MAX_US;

    /* Only near a boundary, so the superloop is not blocked waiting for one. */
    uint32_t boundary = sframe.next_boundary_us;
    if ((int32_t)(boundary - micros()) > (int32_t)(REPORT_BOUNDARY_LEAD_US + uncert))
        return;

    report_attempt_sf = sf;
    if (hop_channel_live(sf, &hop) != 0 || radio_configure((grid = hop_to_grid(hop))) != 0)
        return;

    /* Re-align first; the beacon says the free run was good enough. */
    while (!timebase_elapsed(boundary - DOWNLINK_RX_LEAD_US - uncert)) { }
    info.timeout_us = REPORT_BEACON_WINDOW_US + 2u * uncert;
    if (radio_receive(rx, sizeof(rx), &info) != 0 ||
        beacon_apply(rx, info.len, &sframe, &quiesce, info.start_us, &aligned) != BEACON_OK) {
        /* A counted miss with no record is a rate nobody can date. */
        tlm_emit(TLM_RX_MISS, sf, info.timeout_us, grid, 0u);
        return;
    }
    /* One per cycle: denominator, window, and whether the stamp was captured. */
    tlm_emit(TLM_RX_BEACON, sf, info.timeout_us,
             (info.capture_valid && info.capture_sync) ? 0u : 1u, 0u);
    if (quiesce_active(&quiesce, aligned))
        return;

    /* The anchor is the hub's counter, not this node's guess at it. */
    sf = aligned;
    if ((sf % join_res.report_every) != 0u) {
        tlm_emit(TLM_TX_DENY, sf, TLM_WHY_OFFBEAT, 0u, 0u);
        return;
    }
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
            if (radio_receive(dl, sizeof(dl), &di) == 0 &&
                downlink_open(dl, di.len, &cmd) == 0)
                downlink_apply(sf, &cmd);
        }
    }

    memset(&rep, 0, sizeof(rep));
    rep.rssi_down = beacon_rssi_valid ? beacon_rssi_dbm : 0;
    /* Unconditional while no ADC exists: the wire never claims an unmade reading. */
    rep.flags     = RADIO_REPORT_FLAG_SUPPLY_STALE;
    if (!beacon_rssi_valid)
        rep.flags |= RADIO_REPORT_FLAG_RSSI_STALE;
    rep.supply_mv = 0;
    rep.uptime_s  = timebase_uptime_s();
    rep.ack_seq   = dl_ack_seq;
    rep.ack_cmd   = dl_ack_cmd;
    rep.ack_arg   = dl_ack_arg;

    /* Unreserved is nonce reuse after a reboot. A refusal that skips the
     * top-up denies for ever. */
    uint8_t may_send = (uint8_t)(reserve_covers(sf) != 0);
    if (!may_send) {
        tlm_emit(TLM_TX_DENY, sf, TLM_WHY_RESERVE, 0u, 0u);
        tx_self_silenced = 1;
    }
    if (tx_self_silenced)
        rep.flags |= RADIO_REPORT_FLAG_RESUMED;

    /* Every k is the same channel a second apart: a receiver's own spread. */
    uint8_t k_first = report_opp_all ? 0u : report_opp;
    uint8_t k_last  = report_opp_all ? (uint8_t)(RADIO_SLOT_OPPS - 1u) : report_opp;

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
        if (uplink_seal(sf, slot_n, join_res.dev_id_be, join_res.session,
                        &rep, f) != 0) {
            tlm_emit(TLM_TX_DENY, sf, TLM_WHY_SEAL, 0u, 0u);
            continue;
        }

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
        /* Spent on the frame that carried it, not on the one that was built. */
        tx_self_silenced = 0;
    }

    /* The gap, not the slot. A denied cycle reaches it too, and only it clears one. */
    if (!reserve_known || (int32_t)(sf + RESERVE_TOPUP_AHEAD - reserve_ceiling) >= 0)
        reserve_extend(sf);
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
        uart_tx((uint8_t *)line, (uint16_t)n, 100);
}

static int cmd_tlm(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        tlm_enable(1u);
        out("emitting\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        tlm_enable(0u);
        out("silent\r\n");
        return 0;
    }
    /* The gap in the stream is the evidence; this is the cross-check. */
    out("%s  seq %lu  dropped %lu  ring %u\r\n",
        tlm_enabled() ? "emitting" : "silent", (unsigned long)tlm_seq(),
        (unsigned long)tlm_dropped(), (unsigned)TLM_RING);
    return 0;
}

static int cmd_report(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        if (!join_is_paired()) {
            out("not paired - nothing has granted a slot\r\n");
            return 0;
        }
        report_armed = 1;
        report_attempt_sf = 0;
        /* The moment transmission becomes possible, and the power at it. */
        tlm_emit(TLM_TX_ARM, superframe_now(&sframe),
                 (uint32_t)(int32_t)radio_power_dbm(),
                 report_opp_all ? 0xFFu : report_opp, join_res.report_every);
        /* Here, not in the loop: the first cycle would otherwise refuse itself. */
        if (reserve_extend(superframe_now(&sframe)) != 0)
            out("counter reservation refused - flash will not extend it\r\n");
        if (report_opp_all)
            out("armed: slots %u/%u/%u every %u superframes\r\n",
                (unsigned)join_res.slot,
                (unsigned)(join_res.slot + RADIO_SLOT_STRIDE),
                (unsigned)(join_res.slot + 2u * RADIO_SLOT_STRIDE),
                (unsigned)join_res.report_every);
        else
            out("armed: slot %u every %u superframes\r\n",
                (unsigned)(join_res.slot + report_opp * RADIO_SLOT_STRIDE),
                (unsigned)join_res.report_every);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        report_armed = 0;
        out("disarmed\r\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "band") == 0) {
        if (strcmp(argv[2], "any") == 0)       report_band = 0u;
        else if (strcmp(argv[2], "low") == 0)  report_band = 1u;
        else if (strcmp(argv[2], "high") == 0) report_band = 2u;
        else {
            out("band is any|low|high\r\n");
            return 0;
        }
        out("band %s (join channel is grid %u)\r\n", argv[2],
            (unsigned)radio_join_slot());
        return 0;
    }
    /* Same grant, a later slot: the k=3 geometry used as an instrument.
     * radio_devices_docs/radio/tdma.md */
    if (argc >= 3 && strcmp(argv[1], "opp") == 0) {
        if (strcmp(argv[2], "all") == 0) {
            report_opp_all = 1u;
            for (uint32_t k = 0; k < RADIO_SLOT_OPPS; k++)
                out("slot %u at +%lu us\r\n",
                    (unsigned)(join_res.slot + k * RADIO_SLOT_STRIDE),
                    (unsigned long)RADIO_SLOT_NTH_US(join_res.slot, k));
            return 0;
        }
        uint32_t k = (uint32_t)atoi(argv[2]);
        if (k >= RADIO_SLOT_OPPS) {
            out("k is 0..%u or all\r\n", (unsigned)(RADIO_SLOT_OPPS - 1u));
            return 0;
        }
        report_opp_all = 0u;
        report_opp = (uint8_t)k;
        out("slot %u at +%lu us into the superframe\r\n",
            (unsigned)(join_res.slot + report_opp * RADIO_SLOT_STRIDE),
            (unsigned long)RADIO_SLOT_NTH_US(join_res.slot, report_opp));
        return 0;
    }
    out("opp %s  offset %ld Hz\r\n",
        report_opp_all ? "all" : (report_opp == 0u ? "0" : (report_opp == 1u ? "1" : "2")),
        (long)radio_freq_offset());
    out("band %s (join channel is grid %u)\r\n",
        report_band == 0u ? "any" : (report_band == 1u ? "low" : "high"),
        (unsigned)radio_join_slot());
    /* Rates and residuals are `tlm` records: an accumulator cannot be bracketed. */
    out("%s  every %u superframes  seal refused %lu\r\n",
        report_armed ? "armed" : "disarmed", (unsigned)join_res.report_every,
        (unsigned long)seal_refused);
    if (reserve_known)
        out("counter reserved below %lu\r\n", (unsigned long)reserve_ceiling);
    else
        out("no counter reservation - nothing may be sealed yet\r\n");
    if (report_armed && !tlm_enabled())
        out("armed with telemetry off - nothing is recording the cycles\r\n");
    return 0;
}

static const cli_cmd_t commands[] = {
    {"status", 0, 0, cmd_status, "",                  "uptime, clock, reset cause"},
    {"rng",    0, 1, cmd_rng,    "[count|health]",    "random words from the TRNG"},
    {"radio",  1, 3, cmd_radio,  "<init|chan|crc|preamble|capture|power|offset|tx|rx|gap|rxramp|rxlat|status|reg>", "sub-GHz radio"},
    {"join", 0, 4, cmd_join, "[run|invited|probe <len>|frame|show|hub-key|selftest|stats]", "four-frame pairing, device side"},
    {"vectors", 0, 0, cmd_vectors, "",                  "what this build was compiled against"},
    {"crypto", 1, 1, cmd_crypto, "<gcm|p256|wire|pair>",   "hardware crypto self-test"},
    {"frame",  1, 3, cmd_frame,  "<test|send|recv>",  "sealed frames over the radio"},
    {"time",   0, 3, cmd_time,   "[beacon|quiesce|camp|joinsync|follow|capture [n]]", "the protocol's clock"},
    {"store",  0, 2, cmd_store,  "[new|counter|torn|selftest|erase]", "flash identity and counter mark"},
    {"pair",   1, 3, cmd_pair,   "<keygen|salt|show|hub|device>", "P-256 pairing over the air"},
    {"downlink", 0, 3, cmd_downlink, "[replay|[wide] [n]]", "listen in the downlink region"},
    {"uplink", 0, 2, cmd_uplink, "[selftest [mutate]|replay|again]", "one report in the granted slot"},
    {"hop",    0, 4, cmd_hop,    "[vec|id <sf> <ch>|<sf>]", "the hopping channel plan"},
    {"load",   0, 1, cmd_load,   "[reset]",           "where the CPU's time goes"},
    {"report", 0, 2, cmd_report, "[on|off|opp|band]", "honour the granted uplink cadence"},
    {"tlm",    0, 1, cmd_tlm,    "[on|off]",          "records the device emits on its own"},
    {"recover",0, 2, cmd_recover,"[on|off|park [grid]]", "come back after the counter is lost"},
    {"?",      0, 0, cmd_help,   "",                  "print available commands"},
};

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        out("%-7s %-20s %s\r\n", commands[i].name, commands[i].args, commands[i].help);
    return 0;
}

static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void dispatch(void) {
    char *argv[6];
    int argc = tokenize(cmd, argv, 6);
    resp_len = 0;
    if (argc == 0) {
        out("");
    } else {
        const cli_cmd_t *c = NULL;
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            if (strcmp(argv[0], commands[i].name) == 0) { c = &commands[i]; break; }
        }
        if (c == NULL) {
            out("unknown command: %s\r\n", argv[0]);
        } else if (argc - 1 < c->min_args || argc - 1 > c->max_args) {
            out("usage: %s %s\r\n", c->name, c->args);
        } else {
            c->handler(argc, argv);
        }
    }
    out(">>> ");
    uart_tx( (uint8_t *)resp, (uint16_t)resp_len, 500);
}

void CLI_Init(void) {
    rx_head = rx_tail = 0;
    cmd_len = 0;
    /* The grant lives in flash: a reset must not silently demote a paired device.
     * radio_devices_docs/wl55_device/arch/store.md */
    join_restore(&join_res);
    dl_floor_load();
    __HAL_UART_ENABLE_IT(&hcom_uart[COM1], UART_IT_RXNE);
    HAL_NVIC_SetPriority(LPUART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(LPUART1_IRQn);
    tlm_emit(TLM_BOOT, timebase_uptime_s(), (uint32_t)RCC->CSR,
             RADIO_BITRATE_BPS / 1000u, 0u);
    resp_len = 0;
    out("\r\nwl55 device\r\n>>> ");
    uart_tx( (uint8_t *)resp, (uint16_t)resp_len, 500);
}

/* Called from LPUART1_IRQHandler; keeps the ISR to a byte push. */
void CLI_RxByte(uint8_t b) {
    uint8_t next = (uint8_t)((rx_head + 1u) % CLI_RX_LEN);
    if (next != rx_tail) {
        rx_buf[rx_head] = b;
        rx_head = next;
    }
}

void CLI_Poll(void) {
    while (rx_tail != rx_head) {
        uint8_t c = rx_buf[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1u) % CLI_RX_LEN);

        if (c == '\r' || c == '\n') {
            cmd[cmd_len] = '\0';
            uart_tx( (uint8_t *)"\r\n", 2, 100);
            dispatch();
            cmd_len = 0;
        } else if ((c == 0x7F || c == '\b') && cmd_len > 0) {
            cmd_len--;
            uart_tx( (uint8_t *)"\b \b", 3, 100);
        } else if (c >= 0x20 && c < 0x7F && cmd_len < CLI_CMD_LEN - 1) {
            cmd[cmd_len++] = (char)c;
            uart_tx( &c, 1, 100);
        }
    }
}
