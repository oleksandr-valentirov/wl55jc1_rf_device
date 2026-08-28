/* Records the device emits on its own, because a poll cannot see when.
 * radio_devices_docs/wl55_device/testing/telemetry.md */
#include <stdio.h>

#include "telemetry.h"
#include "clock.h"

typedef struct {
    uint32_t seq;
    uint32_t us;
    uint32_t a, b, c, d;
    uint8_t  kind;
} tlm_rec_t;

/* Names travel with the record, so adding a field shifts nothing. */
typedef struct {
    const char *name;
    const char *key[TLM_FIELDS];
    uint8_t     sgn;      /* bit n: field n prints signed */
} tlm_kind_info_t;

static const tlm_kind_info_t kinds[TLM_KIND_COUNT] = {
    /* build: abbreviated commit, bit 31 set when the tree was dirty. */
    [TLM_BOOT]      = { "boot",     { "up",  "reset", "kbps", "build" }, 0u },
    /* sync: low nibble is sf_sync_t, 0x10 schedulable, 0x20 recovering. */
    [TLM_BEAT]      = { "beat",     { "up",  "sf",    "sync" }, 0u },
    [TLM_SYNC_LOST] = { "sync.lost",{ "sf",  "since", NULL   }, 0u },
    [TLM_SYNC_OK]   = { "sync.ok",  { "sf",  "per",   NULL   }, 0u },
    /* Only when a beacon moved the counter, so silence is the claim. */
    [TLM_SYNC_JUMP] = { "sync.jump",{ "sf",  "was",   "d"    }, 0x4u },
    [TLM_REC_ENTER] = { "rec.enter",{ "sf",  "tier",  NULL   }, 0u },
    [TLM_REC_PARK]  = { "rec.park", { "sf",  "grid",  "hz"   }, 0u },
    [TLM_REC_HIT]   = { "rec.hit",  { "sf",  "grid",  "rssi" }, 0x4u },
    [TLM_REC_DENY]  = { "rec.deny", { "sf",  "grid",  "why"  }, 0u },
    [TLM_REC_EXIT]  = { "rec.exit", { "sf",  "per",   NULL   }, 0u },
    [TLM_TX_UP]     = { "tx.up",    { "sf",  "slot",  "off", "grid" }, 0x4u },
    [TLM_TX_DENY]   = { "tx.deny",  { "sf",  "why",   NULL   }, 0u },
    [TLM_RX_MISS]   = { "rx.miss",  { "sf",  "win",   "grid" }, 0u },
    [TLM_RX_CMD]    = { "rx.cmd",   { "sf",  "cmd",   "seq", "rpt" }, 0u },
    [TLM_RX_BEACON] = { "rx.beacon",{ "sf",  "win",   "fb"   }, 0u },
    /* Every field can differ from the compiled default; that is the point. */
    [TLM_TX_ARM]    = { "tx.arm",  { "sf",  "dbm",   "opp",  "every" }, 0x02u },
    /* why: 0 drawn now, 1 restored from flash, 2 rng, 3 keygen, 4 flash. */
    [TLM_IDENT]     = { "ident",   { "dev", "gen",   "held", "why"   }, 0u },
    [TLM_JOIN_TRY]  = { "join.try",{ "n",   "rc",    "why",  "refused" }, 0u },
    [TLM_JOIN_OK]   = { "join.ok", { "slot","every", "sf"            }, 0u },
    /* The reservation, not a fault: sealing waits, syncing does not. */
    [TLM_TX_HOLD]   = { "tx.hold", { "sf",  "why",   "floor"         }, 0u },
    /* The downlink window's own record: opened and empty said nothing before. */
    [TLM_RX_DL_MISS] = { "rx.dlmiss", { "sf", "win",  "grid", "rc"     }, 0u },
    /* lo is app[0..3] and hi is app[4..5], both little-endian; len says how many. */
    [TLM_RX_APP]    = { "rx.app",  { "sf",  "len",   "lo",   "hi"    }, 0u },
    /* half is the wire's own unit, dbm the reading; the hub measured both. */
    [TLM_RX_LINK]   = { "rx.link", { "sf",  "half",  "dbm"           }, 0x4u },
};

static tlm_rec_t ring[TLM_RING];
static uint8_t   head, tail;
static uint32_t  seq, dropped;
static uint8_t   enabled = 1u;

void tlm_reset(void) {
    head = tail = 0u;
    seq = dropped = 0u;
    enabled = 1u;
}

void tlm_enable(uint8_t on) { enabled = on ? 1u : 0u; }
uint8_t  tlm_enabled(void)  { return enabled; }
uint32_t tlm_dropped(void)  { return dropped; }
uint32_t tlm_seq(void)      { return seq; }

/* Spent whether or not it fits, so a full ring leaves a visible gap. */
void tlm_emit(tlm_kind_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!enabled || (unsigned)kind >= TLM_KIND_COUNT)
        return;

    uint32_t mine = seq++;
    uint8_t next = (uint8_t)((head + 1u) % TLM_RING);

    if (next == tail) {
        dropped++;
        return;
    }
    ring[head].seq  = mine;
    ring[head].us   = micros();
    ring[head].a    = a;
    ring[head].b    = b;
    ring[head].c    = c;
    ring[head].d    = d;
    ring[head].kind = (uint8_t)kind;
    head = next;
}

int tlm_next(char *buf, int max) {
    if (buf == NULL || max < TLM_LINE_MAX || head == tail)
        return 0;

    const tlm_rec_t *r = &ring[tail];
    const tlm_kind_info_t *k = &kinds[r->kind];
    const uint32_t v[TLM_FIELDS] = { r->a, r->b, r->c, r->d };
    int n = snprintf(buf, (size_t)max, "!%lu %lu %s",
                     (unsigned long)r->seq, (unsigned long)r->us, k->name);

    for (int i = 0; i < TLM_FIELDS && n > 0 && n < max; i++) {
        if (k->key[i] == NULL)
            break;
        int w = (k->sgn & (1u << i))
              ? snprintf(buf + n, (size_t)(max - n), " %s=%ld",
                         k->key[i], (long)(int32_t)v[i])
              : snprintf(buf + n, (size_t)(max - n), " %s=%lu",
                         k->key[i], (unsigned long)v[i]);
        if (w < 0)
            break;
        n += w;
    }
    if (n < 0 || n > max - 3)
        n = max - 3;
    n += snprintf(buf + n, (size_t)(max - n), "\r\n");

    tail = (uint8_t)((tail + 1u) % TLM_RING);
    return n;
}
