/* The format checked against written-out lines: a round trip agrees with any.
 * radio_devices_docs/wl55_device/testing/telemetry.md */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telemetry.h"
#include "timebase.h"

void host_clock_set(uint32_t us);
void host_clock_advance(uint32_t us);

static int failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("FAIL %s:%d  ", __func__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

static void eqline(const char *want) {
    char buf[TLM_LINE_MAX];
    int n = tlm_next(buf, sizeof(buf));

    if (n == 0) {
        printf("FAIL  expected \"%s\", ring was empty\n", want);
        failures++;
        return;
    }
    CHECK(strcmp(buf, want) == 0, "got \"%s\" want \"%s\"", buf, want);
}

/* Every field name and both signednesses, written out. */
static void test_format(void) {
    tlm_reset();
    host_clock_set(1000u);

    tlm_emit(TLM_BOOT, 0u, 0x1C010600u, 50u, 0u);
    eqline("!0 1000 boot up=0 reset=469829120 kbps=50\r\n");

    /* The arm record is the one that can differ from the compiled default. */
    tlm_emit(TLM_TX_ARM, 643552u, (uint32_t)(int32_t)-17, 0xFFu, 8u);
    eqline("!1 1000 tx.arm sf=643552 dbm=-17 opp=255 every=8\r\n");

    host_clock_advance(500u);
    tlm_emit(TLM_REC_HIT, 555789u, 10u, (uint32_t)(int32_t)-72, 0u);
    eqline("!2 1500 rec.hit sf=555789 grid=10 rssi=-72\r\n");

    host_clock_advance(1u);
    tlm_emit(TLM_REC_PARK, 555790u, 6u, 865700000u, 0u);
    eqline("!3 1501 rec.park sf=555790 grid=6 hz=865700000\r\n");

    /* A NULL key ends the line rather than printing a zero. */
    host_clock_advance(1u);
    tlm_emit(TLM_REC_EXIT, 555791u, 2004571u, 999u, 0u);
    eqline("!4 1502 rec.exit sf=555791 per=2004571\r\n");

    host_clock_advance(1u);
    tlm_emit(TLM_TX_UP, 7u, 66u, (uint32_t)(int32_t)-1234, 23u);
    eqline("!5 1503 tx.up sf=7 slot=66 off=-1234 grid=23\r\n");

    /* A correction backwards must read as negative, not as four billion. */
    host_clock_advance(1u);
    tlm_emit(TLM_SYNC_JUMP, 561256u, 557692u, (uint32_t)(int32_t)3564, 0u);
    eqline("!6 1504 sync.jump sf=561256 was=557692 d=3564\r\n");
    host_clock_advance(1u);
    tlm_emit(TLM_SYNC_JUMP, 100u, 110u, (uint32_t)(int32_t)-10, 0u);
    eqline("!7 1505 sync.jump sf=100 was=110 d=-10\r\n");

    /* The per-cycle denominator that replaced `report cycles`. */
    host_clock_advance(1u);
    tlm_emit(TLM_RX_BEACON, 610664u, 71860u, 1u, 0u);
    eqline("!8 1506 rx.beacon sf=610664 win=71860 fb=1\r\n");

    host_clock_advance(1u);
    tlm_emit(TLM_RX_CMD, 618600u, 1u, 1u, 0u);
    eqline("!9 1507 rx.cmd sf=618600 cmd=1 seq=1 rpt=0\r\n");

    /* Every `why` renders, so a new code cannot arrive as a bare number. */
    host_clock_advance(1u);
    tlm_emit(TLM_TX_DENY, 610672u, TLM_WHY_OFFBEAT, 0u, 0u);
    eqline("!10 1508 tx.deny sf=610672 why=7\r\n");

    CHECK(tlm_next(NULL, 0) == 0, "a drained ring still produced a line");
}

/* A dropped record still spends its number, so loss is in the stream. */
static void test_overflow_leaves_a_gap(void) {
    char buf[TLM_LINE_MAX];
    tlm_reset();
    host_clock_set(0u);

    for (uint32_t i = 0; i < TLM_RING + 5u; i++)
        tlm_emit(TLM_REC_ENTER, i, 0u, 0u, 0u);

    CHECK(tlm_dropped() == 6u, "dropped %lu, want 6", (unsigned long)tlm_dropped());
    CHECK(tlm_seq() == TLM_RING + 5u, "seq %lu", (unsigned long)tlm_seq());

    uint32_t last = 0;
    int lines = 0;
    while (tlm_next(buf, sizeof(buf)) != 0) {
        unsigned long s = strtoul(buf + 1, NULL, 10);
        CHECK(lines == 0 || s == last + 1u, "seq jumped inside the ring");
        last = (uint32_t)s;
        lines++;
    }
    CHECK(lines == TLM_RING - 1, "drained %d lines", lines);
    /* The gap is at the end, between the last drained seq and tlm_seq(). */
    CHECK(tlm_seq() - last - 1u == 6u, "the gap does not name the six losses");
}

static void test_disabled_emits_nothing(void) {
    char buf[TLM_LINE_MAX];
    tlm_reset();
    tlm_enable(0u);
    tlm_emit(TLM_BEAT, 1u, 2u, 3u, 0u);
    CHECK(tlm_next(buf, sizeof(buf)) == 0, "a disabled ring produced a line");
    /* And it does not spend a sequence number either, or every disabled window
     * would read as loss. */
    CHECK(tlm_seq() == 0u, "seq moved while disabled");
    tlm_enable(1u);
}

/* A line must never exceed the buffer the console writes in one go. */
static void test_line_fits(void) {
    char buf[TLM_LINE_MAX];
    tlm_reset();
    host_clock_set(0xFFFFFFFFu);
    for (int k = 0; k < TLM_KIND_COUNT; k++)
        tlm_emit((tlm_kind_t)k, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                 0xFFFFFFFFu);
    int n;
    while ((n = tlm_next(buf, sizeof(buf))) != 0) {
        CHECK(n < TLM_LINE_MAX, "line of %d bytes", n);
        CHECK(buf[0] == '!', "line does not start with the sentinel");
        CHECK(strstr(buf, "\r\n") != NULL, "line is not terminated");
    }
}

int main(void) {
    test_format();
    test_overflow_leaves_a_gap();
    test_disabled_emits_nothing();
    test_line_fits();

    printf(failures ? "%d telemetry check(s) failed\n" : "all telemetry checks passed\n",
           failures);
    return failures ? 1 : 0;
}
