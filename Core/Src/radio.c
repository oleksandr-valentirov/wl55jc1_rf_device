/* Sub-GHz radio: PHY profile shared with the hub, plus bring-up plumbing.
 *
 * The STM32WL radio is an SX126x core reached over an internal SPI, so there is
 * no wiring to get wrong - if it does not answer, the fault is configuration,
 * not soldering. The settings below mirror the hub's RFM69 profile register for
 * register, because the two have to demodulate each other, not merely work. */
#include <string.h>

#include "main.h"
#include "radio.h"
#include "radio_phy.h"
#include "load.h"
#include "crypto.h"
#include "timebase.h"
#include "stm32wlxx_nucleo_radio.h"

extern SUBGHZ_HandleTypeDef hsubghz;
extern RNG_HandleTypeDef hrng;

/* Carrier, rate, deviation, sync word, CRC and the grid come from
 * Common/inc/radio_phy.h - the contract both sides compile, so a value defined
 * here would be one each side could be internally consistent about and still
 * disagree on. Only this part's opcode encodings are local. */

#define XTAL_HZ                 32000000u

/* SX126x opcode arguments that the HAL does not name. */
#define PACKET_TYPE_GFSK        0x00u
#define PULSE_SHAPE_BT_0_5      0x09u
#define RX_BW_117300            0x0Bu   /* nearest step above the hub's 100 kHz */
#define PREAMBLE_DETECT_16BIT   0x05u
#define ADDRCOMP_OFF            0x00u
#define PACKET_VARIABLE_LEN     0x01u
#define CRC_OFF                 0x01u
#define CRC_2_BYTE              0x02u
/* The SX1231/RFM69 transmits its CRC-16 inverted relative to this part's plain
 * two-byte mode. Seed and polynomial already agree - both default to 0x1D0F
 * over 0x1021 - so inversion is the only degree of freedom left, and it is the
 * one nothing on either side could observe: a CRC mismatch is discarded in
 * hardware and increments no counter anywhere. */
#define CRC_2_BYTE_INV          0x06u
#define WHITENING_OFF           0x00u
#define STDBY_RC                0x00u
#define REGULATOR_DCDC          0x01u

/* The 32 MHz reference on this board is a TCXO whose supply the radio itself
 * switches, so the chip cannot start its oscillator until it is told to power
 * it. 1.7 V is what the Nucleo fits. The timeout is in 15.625 us steps. */
#define TCXO_CTRL_1_7V          0x01u
/* 2 ms. The reference design's 10 ms was never measured on this board and is the
 * whole of the RX entry latency; sweeping it found the link and the carrier
 * unchanged down to 500 us, so this keeps 4x margin over the lowest verified
 * setting. Cold start is the untested axis - TCXO settling is temperature
 * dependent and this bench is at room temperature. */
#define TCXO_STARTUP_STEPS      128u
#define CALIBRATE_ALL           0x7Fu

#define IRQ_TX_DONE             0x0001u
#define IRQ_RX_DONE             0x0002u
#define IRQ_PREAMBLE_DET        0x0004u
#define IRQ_SYNC_VALID          0x0008u
#define IRQ_CRC_ERR             0x0040u
#define IRQ_TIMEOUT             0x0200u

#define REG_SYNCWORD_BASE       0x06C0u
#define REG_WHITENING_MSB       0x06B8u

#define RADIO_MAX_PAYLOAD       255u
#define TX_BASE_ADDR            0x00u
#define RX_BASE_ADDR            0x80u

/* Same four bytes the hub writes into RegSyncValue1..4. */
static const uint8_t sync_word[RADIO_SYNC_BYTES] = RADIO_SYNC_WORD;

/* Bench traffic gets its own frequency and its own sync word, agreed with the
 * hub session. Sharing either is not a collision risk but a diagnosis risk: a
 * matching sync word makes each side's decoder accept the other's frames, and
 * 866.5 MHz is the one channel a device with no key has no alternative to. */
#define RADIO_BENCH_HZ          869500000u

/* Wide enough for measured carrier error, narrow enough to stay in band. */
#define RADIO_FREQ_OFFSET_MAX_HZ  100000
static const uint8_t sync_word_bench[4] = {'b', 'e', 'n', 'c'};

static uint8_t configured;
static uint8_t bench_mode;
static uint8_t crc_type = CRC_2_BYTE_INV;
static uint16_t preamble_bits = RADIO_PREAMBLE_BYTES * 8u;
static uint8_t current_slot;
/* -17..+14 on the low-power PA, so a calibration can be re-run at another
 * level. */
static int8_t tx_dbm = 14;
#define TX_RAMP_200US   0x04u

static int configure(uint8_t slot, int want_bench);

/* Settable so the 10 ms can be measured rather than inherited: it is the whole
 * of the RX entry latency and most of the TX ramp, and nothing on this board
 * says the Nucleo's TCXO actually needs that long. */
static uint32_t tcxo_steps = TCXO_STARTUP_STEPS;

static int cmd_set(SUBGHZ_RadioSetCmd_t op, uint8_t *buf, uint16_t len) {
    load_enter(LOAD_RADIO_SPI);
    int rc = (HAL_SUBGHZ_ExecSetCmd(&hsubghz, op, buf, len) == HAL_OK) ? 0 : -1;
    load_exit();
    return rc;
}

static int cmd_get(SUBGHZ_RadioGetCmd_t op, uint8_t *buf, uint16_t len) {
    load_enter(LOAD_RADIO_SPI);
    int rc = (HAL_SUBGHZ_ExecGetCmd(&hsubghz, op, buf, len) == HAL_OK) ? 0 : -1;
    load_exit();
    return rc;
}

int radio_get_status(uint8_t *status) {
    return cmd_get(RADIO_GET_STATUS, status, 1);
}

int radio_read_reg(uint16_t addr, uint8_t *value) {
    return (HAL_SUBGHZ_ReadRegister(&hsubghz, addr, value) == HAL_OK) ? 0 : -1;
}

int radio_write_reg(uint16_t addr, uint8_t value) {
    return (HAL_SUBGHZ_WriteRegister(&hsubghz, addr, value) == HAL_OK) ? 0 : -1;
}

int radio_get_error(uint16_t *err) {
    uint8_t buf[2] = {0};
    if (cmd_get(RADIO_GET_ERROR, buf, sizeof(buf)) != 0)
        return -1;
    *err = (uint16_t)((buf[0] << 8) | buf[1]);
    return 0;
}

int radio_standby(void) {
    uint8_t cfg = STDBY_RC;
    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
    return cmd_set(RADIO_SET_STANDBY, &cfg, 1);
}

int radio_rng_word(uint32_t *out) {
    /* One checked implementation, in crypto.c, rather than two draws with
     * different ideas about what a seed error means. */
    return crypto_rng_word(out);
}

uint32_t radio_slot_hz(uint8_t slot) {
    return RADIO_CH_BASE_HZ + (slot % RADIO_GRID_COUNT) * RADIO_CH_SPACING_HZ;
}

uint8_t radio_join_slot(void) {
    return RADIO_JOIN_SLOT;
}

/* Applied here rather than at the call sites, so no tuning path can miss it. */
static int32_t freq_offset_hz;

int radio_set_freq_offset(int32_t hz) {
    if (hz < -RADIO_FREQ_OFFSET_MAX_HZ || hz > RADIO_FREQ_OFFSET_MAX_HZ)
        return -1;
    freq_offset_hz = hz;
    if (!configured)
        return 0;
    return configure(current_slot, bench_mode);
}

int32_t radio_freq_offset(void) {
    return freq_offset_hz;
}

static int set_frequency(uint32_t hz) {
    /* PLL step is XTAL/2^25; the multiply is done in 64 bits to keep the
     * rounding honest at 865 MHz, where a 32-bit intermediate overflows. */
    uint64_t raw = (((uint64_t)((int64_t)hz + freq_offset_hz)) << 25) / XTAL_HZ;
    uint8_t buf[4] = {(uint8_t)(raw >> 24), (uint8_t)(raw >> 16),
                      (uint8_t)(raw >> 8), (uint8_t)raw};
    return cmd_set(RADIO_SET_RFFREQUENCY, buf, sizeof(buf));
}

/* Leaving bench mode is a reconfiguration, not a retune. This cleared the flag
 * and changed only the frequency, so the radio sat on a protocol channel with
 * the bench sync word while radio_bench_mode() reported protocol - a flag
 * broader than what it covered, and on air it is a frame nobody decodes. */
int radio_set_channel(uint8_t slot) {
    if (bench_mode)
        return radio_configure(slot);
    return set_frequency(radio_slot_hz(slot));
}

/* Settable so a variant can be swept inside one pairing window instead of one
 * rebuild-and-flash per guess. */
int radio_set_crc(const char *mode) {
    if (strcmp(mode, "off") == 0)        crc_type = CRC_OFF;
    else if (strcmp(mode, "2") == 0)     crc_type = CRC_2_BYTE;
    else if (strcmp(mode, "2inv") == 0)  crc_type = CRC_2_BYTE_INV;
    else return -1;
    if (!configured)
        return 0;
    return configure(current_slot, bench_mode);
}

/* What the chip holds, not what we composed - the SPI write is inside this and
 * a struct dump is not. */
int radio_read_tx_buffer(uint8_t *out, uint8_t len) {
    if (!configured || out == NULL || len == 0u)
        return -1;
    return HAL_SUBGHZ_ReadBuffer(&hsubghz, TX_BASE_ADDR, out, len) == HAL_OK ? 0 : -1;
}

/* The PHY owns its own overhead: the preamble is settable and the CRC is two
 * bytes or none, so a caller computing air time from the payload alone would be
 * wrong the moment either moves. */
/* The contract's macro is fixed at the default preamble and a CRC that is on;
 * this function follows both when they move. They must still agree where the
 * macro is valid, or one of them is wrong about the same wire. */
_Static_assert(RADIO_PHY_OVERHEAD_B ==
                   RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES + 1u + RADIO_CRC_BYTES,
               "PHY overhead disagrees with its own parts");

/* Takes the preamble as an argument so a value can be costed before it is set. */
static uint32_t air_time_us(uint8_t preamble_b, uint8_t payload_len) {
    uint32_t bytes = (uint32_t)preamble_b + sizeof(sync_word) + 1u
                     + (uint32_t)payload_len
                     + ((crc_type == CRC_OFF) ? 0u : 2u);
    return bytes * 8u * 1000000u / RADIO_BITRATE_BPS;
}

uint32_t radio_air_time_us(uint8_t payload_len) {
    return air_time_us((uint8_t)(preamble_bits / 8u), payload_len);
}

/* The register range is not the limit: the slot is.
 * radio_devices_docs/radio/phy.md */
int radio_set_preamble(uint8_t bytes) {
    if (bytes < 2u || bytes > 32u)
        return -1;
    if (air_time_us(bytes, RADIO_UPLINK_BYTES) > RADIO_SLOT_US)
        return -1;
    uint16_t was = preamble_bits;
    preamble_bits = (uint16_t)bytes * 8u;
    if (!configured)
        return 0;
    /* The variable goes back with a refused reconfigure, or air time lies. */
    if (configure(current_slot, bench_mode) != 0) {
        preamble_bits = was;
        return -1;
    }
    return 0;
}

uint8_t radio_preamble_bytes(void) {
    return (uint8_t)(preamble_bits / 8u);
}

const char *radio_crc_name(void) {
    return crc_type == CRC_OFF ? "off" :
           crc_type == CRC_2_BYTE ? "2-byte" : "2-byte-inverted";
}

int radio_bench_mode(void) {
    return bench_mode;
}

static int set_modulation(void) {
    /* BR register is 32 * XTAL / bitrate, Fdev is deviation in PLL steps. */
    uint32_t br = (uint32_t)(((uint64_t)XTAL_HZ * 32u) / RADIO_BITRATE_BPS);
    uint32_t fdev = (uint32_t)(((uint64_t)RADIO_DEVIATION_HZ << 25) / XTAL_HZ);
    uint8_t buf[8] = {
        (uint8_t)(br >> 16), (uint8_t)(br >> 8), (uint8_t)br,
        PULSE_SHAPE_BT_0_5, RX_BW_117300,
        (uint8_t)(fdev >> 16), (uint8_t)(fdev >> 8), (uint8_t)fdev
    };
    return cmd_set(RADIO_SET_MODULATIONPARAMS, buf, sizeof(buf));
}

static int set_packet_params(uint8_t payload_len) {
    /* Preamble length is in bits here and in bytes on the RFM69 - the hub's
     * four bytes are these thirty-two. Settable because the far end's bit
     * synchroniser needs some number of them and nobody has measured which. */
    uint8_t buf[9] = {
        (uint8_t)(preamble_bits >> 8), (uint8_t)preamble_bits,
        PREAMBLE_DETECT_16BIT,
        (uint8_t)(sizeof(sync_word) * 8u),
        ADDRCOMP_OFF,
        PACKET_VARIABLE_LEN,
        payload_len,
        crc_type,
        WHITENING_OFF
    };
    return cmd_set(RADIO_SET_PACKETPARAMS, buf, sizeof(buf));
}

/* dio1 selects which of mask reaches the NVIC. radio_devices_docs/wl55_device/radio/timebase.md */
static int set_irq_mask(uint16_t mask, uint16_t dio1) {
    uint8_t buf[8] = {(uint8_t)(mask >> 8), (uint8_t)mask,
                      (uint8_t)(dio1 >> 8), (uint8_t)dio1, 0, 0, 0, 0};
    return cmd_set(RADIO_CFG_DIOIRQ, buf, sizeof(buf));
}

volatile uint32_t radio_capture_us;   /* written by SUBGHZ_Radio_IRQHandler */
volatile uint8_t  radio_capture_seen;

/* A TIM2 read and nothing else; reading the status would cost SPI. */
void SUBGHZ_Radio_IRQHandler(void) {
    radio_capture_us = micros();
    radio_capture_seen = 1u;
    HAL_NVIC_DisableIRQ(SUBGHZ_Radio_IRQn);
}

/* One of the two reaches the NVIC, so the ISR needs no status read.
 * radio_devices_docs/wl55_device/radio/timebase.md */
static uint16_t capture_irq = IRQ_SYNC_VALID;

static int apply_irq_mask(void) {
    return set_irq_mask(IRQ_TX_DONE | IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_TIMEOUT |
                        IRQ_PREAMBLE_DET | IRQ_SYNC_VALID, capture_irq);
}

int radio_set_capture(const char *src) {
    uint16_t want;
    if (strcmp(src, "sync") == 0)
        want = IRQ_SYNC_VALID;
    else if (strcmp(src, "preamble") == 0)
        want = IRQ_PREAMBLE_DET;
    else
        return -1;
    capture_irq = want;
    return configured ? apply_irq_mask() : 0;
}

const char *radio_capture_name(void) {
    return capture_irq == IRQ_SYNC_VALID ? "sync" : "preamble";
}

static int set_tx_params(void) {
    uint8_t buf[2] = {(uint8_t)tx_dbm, TX_RAMP_200US};
    return cmd_set(RADIO_SET_TXPARAMS, buf, sizeof(buf));
}

int radio_set_power(int dbm) {
    if (dbm < -17 || dbm > 14)
        return -1;
    tx_dbm = (int8_t)dbm;
    return configured ? set_tx_params() : 0;
}

int radio_power_dbm(void) {
    return tx_dbm;
}

static int clear_irq(uint16_t mask) {
    uint8_t buf[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
    return cmd_set(RADIO_CLR_IRQSTATUS, buf, sizeof(buf));
}

static int get_irq(uint16_t *status) {
    uint8_t buf[2] = {0};
    /* HAL_SUBGHZ_ExecGetCmd already flushes the chip status byte, so what lands
     * in the buffer is the payload alone - no leading byte to skip. */
    if (cmd_get(RADIO_GET_IRQSTATUS, buf, sizeof(buf)) != 0)
        return -1;
    *status = (uint16_t)((buf[0] << 8) | buf[1]);
    return 0;
}

static int configure(uint8_t slot, int want_bench) {
    uint8_t buf[8];

    if (radio_standby() != 0)
        return -1;

    buf[0] = REGULATOR_DCDC;    /* the Nucleo fits the SMPS; LDO wastes it */
    if (cmd_set(RADIO_SET_REGULATORMODE, buf, 1) != 0)
        return -1;

    buf[0] = TCXO_CTRL_1_7V;
    buf[1] = (uint8_t)(tcxo_steps >> 16);
    buf[2] = (uint8_t)(tcxo_steps >> 8);
    buf[3] = (uint8_t)tcxo_steps;
    if (cmd_set(RADIO_SET_TCXOMODE, buf, 4) != 0)
        return -1;

    /* Every calibration before this point ran against an oscillator that was
     * not powered, so they all have to be redone. */
    buf[0] = CALIBRATE_ALL;
    if (cmd_set(RADIO_CALIBRATE, buf, 1) != 0)
        return -1;
    delay_us_poll(5000u);
    /* ClrError takes two bytes; feeding it one leaves the register holding
     * whatever the next byte on the bus happened to be. */
    buf[0] = 0x00; buf[1] = 0x00;
    if (cmd_set(RADIO_CLR_ERROR, buf, 2) != 0)
        return -1;

    buf[0] = PACKET_TYPE_GFSK;
    if (cmd_set(RADIO_SET_PACKETTYPE, buf, 1) != 0)
        return -1;

    if (set_frequency(want_bench ? RADIO_BENCH_HZ : radio_slot_hz(slot)) != 0)
        return -1;
    if (set_modulation() != 0)
        return -1;
    if (set_packet_params(RADIO_MAX_PAYLOAD) != 0)
        return -1;

    const uint8_t *sync = want_bench ? sync_word_bench : sync_word;
    for (uint8_t i = 0; i < sizeof(sync_word); i++) {
        if (radio_write_reg((uint16_t)(REG_SYNCWORD_BASE + i), sync[i]) != 0)
            return -1;
    }

    buf[0] = TX_BASE_ADDR;
    buf[1] = RX_BASE_ADDR;
    if (cmd_set(RADIO_SET_BUFFERBASEADDRESS, buf, 2) != 0)
        return -1;

    /* 14 dBm on the low-power PA, matching the hub's 13 dBm within a step.
     * Fields are paDutyCycle, hpMax, deviceSel, paLut - deviceSel must be 1 for
     * the LP PA, or the chip drives the HP pin while the board's switch routes
     * the LP path and almost nothing reaches the antenna. */
    buf[0] = 0x04; buf[1] = 0x00; buf[2] = 0x01; buf[3] = 0x01;
    if (cmd_set(RADIO_SET_PACONFIG, buf, 4) != 0)
        return -1;
    if (set_tx_params() != 0)
        return -1;

    if (apply_irq_mask() != 0)
        return -1;
    if (clear_irq(0xFFFFu) != 0)
        return -1;

    configured = 1;
    bench_mode = (uint8_t)(want_bench != 0);
    current_slot = slot;
    return 0;
}

int radio_configure(uint8_t slot) {
    return configure(slot, 0);
}

int radio_configure_bench(void) {
    return configure(0, 1);
}

uint32_t radio_bench_hz(void) {
    return RADIO_BENCH_HZ;
}

uint32_t radio_tcxo_us(void) {
    return (tcxo_steps * 1000u) / 64u;
}

int radio_set_tcxo_us(uint32_t us) {
    uint32_t steps = (us * 64u) / 1000u;
    if (steps == 0u || steps > 0x00FFFFFFu)
        return -1;
    tcxo_steps = steps;
    /* The chip latches the mode at configure time, so nothing changes until the
     * whole PHY is set up again - and re-running it also clears the errors a
     * previous too-short setting latched. */
    return configure(0, bench_mode);
}

/* Every wait here is bounded. The old hub driver polled DIO without a timeout,
 * which turns any missed interrupt into a hung core rather than a failed call. */
/* The poll itself is SPI traffic, so the two are charged separately: what is
 * left under radio-wait is the part a DIO interrupt or a sleep could recover. */
static int wait_irq(uint16_t wanted, uint32_t timeout_us, uint16_t *got) {
    uint32_t deadline = micros() + timeout_us;
    int rc;

    load_enter(LOAD_RADIO_WAIT);
    for (;;) {
        uint16_t irq = 0;
        if (get_irq(&irq) != 0) { rc = -1; break; }
        if (irq & (wanted | IRQ_TIMEOUT)) { *got = irq; rc = 0; break; }
        if (timebase_elapsed(deadline)) { rc = -4; break; }
    }
    load_exit();
    return rc;
}

int radio_send(const uint8_t *payload, uint8_t len, uint32_t *air_us) {
    uint8_t buf[8];
    uint16_t irq = 0;

    if (!configured || len == 0)
        return -1;
    if (clear_irq(0xFFFFu) != 0)
        return -1;
    if (set_packet_params(len) != 0)
        return -1;
    load_enter(LOAD_RADIO_SPI);
    HAL_StatusTypeDef wb = HAL_SUBGHZ_WriteBuffer(&hsubghz, TX_BASE_ADDR, (uint8_t *)payload, len);
    load_exit();
    if (wb != HAL_OK)
        return -1;

    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_RFO_LP);
    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00;   /* no chip-side timeout */
    uint32_t t0 = micros();
    if (cmd_set(RADIO_SET_TX, buf, 3) != 0) {
        BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
        return -1;
    }

    int rc = wait_irq(IRQ_TX_DONE, 500000u, &irq);
    if (air_us != NULL)
        *air_us = micros() - t0;
    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
    if (rc != 0)
        return rc;
    clear_irq(0xFFFFu);
    return (irq & IRQ_TX_DONE) ? 0 : -2;
}

static int receive_from(uint8_t *payload, uint8_t max_len, radio_rx_info_t *info,
                        const uint32_t *start_us) {
    uint8_t buf[8];
    uint16_t irq = 0;

    if (!configured)
        return -1;
    /* The caller owns timeout_us; clearing the whole struct would zero it and
     * make every receive expire before the radio has even started. */
    uint32_t timeout_us = info->timeout_us;
    memset(info, 0, sizeof(*info));
    info->timeout_us = timeout_us;
    if (clear_irq(0xFFFFu) != 0)
        return -1;
    if (set_packet_params(RADIO_MAX_PAYLOAD) != 0)
        return -1;

    radio_capture_seen = 0u;
    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 5, 0);
    /* The pending bit latches while disabled; without this the enable re-enters. */
    HAL_NVIC_ClearPendingIRQ(SUBGHZ_Radio_IRQn);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_RX);
    /* Everything above is SPI the caller should not pay for at the appointment:
     * only the command itself has to land on time. */
    if (start_us != NULL)
        while (!timebase_elapsed(*start_us))
            ;
    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00;   /* single receive, no timeout */
    if (cmd_set(RADIO_SET_RX, buf, 3) != 0) {
        BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
        return -1;
    }

    int rc = wait_irq(IRQ_RX_DONE | IRQ_CRC_ERR, timeout_us, &irq);
    info->done_us = micros();
    HAL_NVIC_DisableIRQ(SUBGHZ_Radio_IRQn);
    info->capture_valid = radio_capture_seen;
    info->capture_us    = radio_capture_us;
    info->capture_sync  = (uint8_t)(capture_irq == IRQ_SYNC_VALID);
    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
    radio_standby();
    if (rc != 0)
        return rc;

    if (irq & IRQ_CRC_ERR) {
        info->crc_error = 1;
        clear_irq(0xFFFFu);
        return -3;
    }
    if (!(irq & IRQ_RX_DONE)) {
        clear_irq(0xFFFFu);
        return -2;
    }

    uint8_t st[2] = {0};
    if (cmd_get(RADIO_GET_RXBUFFERSTATUS, st, sizeof(st)) != 0)
        return -1;
    uint8_t len = st[0];
    uint8_t offset = st[1];
    if (len > max_len)
        len = max_len;
    load_enter(LOAD_RADIO_SPI);
    HAL_StatusTypeDef rb = HAL_SUBGHZ_ReadBuffer(&hsubghz, offset, payload, len);
    load_exit();
    if (rb != HAL_OK)
        return -1;

    uint8_t ps[3] = {0};
    if (cmd_get(RADIO_GET_PACKETSTATUS, ps, sizeof(ps)) == 0)
        info->rssi_dbm = -(int16_t)(ps[2] / 2);   /* RssiAvg, in half-dB steps */
    info->len = len;
    /* A preamble edge is not a frame start: it lands after the preamble.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    info->start_us = (info->capture_valid && info->capture_sync)
                   ? info->capture_us - RADIO_PRE_SYNC_US
                   : info->done_us - RADIO_FRAME_AIR_US(len);
    clear_irq(0xFFFFu);
    return 0;
}

int radio_receive(uint8_t *payload, uint8_t max_len, radio_rx_info_t *info) {
    return receive_from(payload, max_len, info, NULL);
}

int radio_receive_at(uint8_t *payload, uint8_t max_len, radio_rx_info_t *info,
                     uint32_t start_us) {
    return receive_from(payload, max_len, info, &start_us);
}

int radio_rx_ramp_probe(uint32_t timeout_us, uint32_t *span_us) {
    uint8_t buf[3];
    uint16_t irq = 0;

    if (!configured || timeout_us == 0u)
        return -1;
    if (clear_irq(0xFFFFu) != 0)
        return -1;
    if (set_packet_params(RADIO_MAX_PAYLOAD) != 0)
        return -1;

    /* 15.625 us per step is 1/64 us inverted, so the conversion is exact in
     * integers and the requested figure is not quietly rounded away. */
    uint32_t steps = (timeout_us * 64u) / 1000u;
    if (steps == 0u || steps > 0x00FFFFFFu)
        return -1;

    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_RX);
    buf[0] = (uint8_t)(steps >> 16);
    buf[1] = (uint8_t)(steps >> 8);
    buf[2] = (uint8_t)steps;
    uint32_t t0 = micros();
    if (cmd_set(RADIO_SET_RX, buf, 3) != 0) {
        BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
        return -1;
    }

    int rc = wait_irq(IRQ_TIMEOUT, timeout_us * 2u + 200000u, &irq);
    *span_us = micros() - t0;
    BSP_RADIO_ConfigRFSwitch(RADIO_SWITCH_OFF);
    radio_standby();
    clear_irq(0xFFFFu);
    return (rc == 0 && (irq & IRQ_TIMEOUT)) ? 0 : -2;
}
