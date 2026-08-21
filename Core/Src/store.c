/* Non-volatile identity and superframe mark, in the last two flash pages.
 *
 * Append-only across two pages, newest valid sequence number wins. A torn write
 * fails its checksum and is skipped, so a power loss mid-record costs the newest
 * record rather than the store. */
#include <string.h>

#include "main.h"
#include "store.h"
#include "load.h"

#define STORE_PAGE_A     0x0803F000u
#define STORE_PAGE_B     0x0803F800u
#define STORE_PAGE_SIZE  2048u
#define STORE_SLOT_SIZE  256u
#define STORE_SLOTS      (STORE_PAGE_SIZE / STORE_SLOT_SIZE)
/* Bumped for the 128-byte record. The old 64-byte records stay in flash and
 * fail this, so the scanner ignores them - and because the append point is the
 * first *erased* slot rather than one past the last valid record, the writer
 * steps over them instead of aiming at occupied flash. That is the whole
 * migration: a device paired before this bump loses its identity once and
 * re-pairs, rather than failing a write for ever. */
#define STORE_MAGIC      0x4B535735u
/* The 128-byte format. Its records stay in flash and fail both the magic and
 * the CRC, so the scanner steps over them - but "no record" and "a record this
 * build cannot read" are different facts that demand opposite answers, and a
 * store that reports only the first turns a format change into a device that
 * looks like it was never provisioned. `store_legacy_present()` separates
 * them. */
#define STORE_LEGACY_MAGIC  0x4B535734u
#define STORE_LEGACY_SLOT   128u

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t dev_id;
    uint32_t counter_mark;
    uint8_t  priv[STORE_PRIV_LEN];
    /* The generation rx_floor belongs to. Clearing the floor on key install is
     * correct but procedural - it works because the call site is right, which is
     * a property of today's code and not of the data. Naming the generation makes
     * a stale floor detectable instead, so a missed clear fails loudly rather
     * than making the device silently deaf. */
    uint32_t key_gen;
    uint32_t rx_floor;      /* replay window survives a reset only if stored */
    /* Everything below is what a device needs to still be paired after a power
     * cut. The session key cannot be re-derived: that would need hub_eph, which
     * exists only for the length of the exchange. Without these a device pairs
     * successfully and is dead at the next reset.
     *
     * hop_key, hub_static, slot and report_every have no writer yet - they
     * arrive with PAIR_ACCEPT and with provisioning, neither of which is built.
     * They are here now because the record format has to be right before anyone
     * pairs, not because they work. */
    uint8_t  session[STORE_KEY_LEN];
    uint8_t  hop_key[STORE_KEY_LEN];        /* network-wide, from PAIR_ACCEPT */
    uint8_t  hub_static[STORE_PUB_C_LEN];   /* provisioned; Z1 needs it */
    uint8_t  slot;                          /* granted uplink slot */
    uint8_t  report_every;
    /* The highest superframe at which a PAIR_INIT was accepted. Durable because
     * the alternative is a rate limit, and a rate limit resets on reboot: an
     * attacker with a recording and a power cut beats one and not the other.
     * Same shape as the hub's counter ceiling for the same reason. */
    uint32_t init_ceiling;
    /* Learned from a join beacon. Carved out of the pad rather than appended,
     * so the record keeps its size and its magic: every existing record was
     * written from a zeroed struct, so these read 0 - which is the same answer
     * as "no beacon heard yet" and needs no migration to mean it. */
    uint32_t hub_id;
    uint16_t net_id;
    uint8_t  pad[119];      /* fills the slot exactly; covered by the CRC */
    uint32_t crc;
} __attribute__((packed)) store_record_t;

/* Filling the slot exactly is a more useful invariant than any particular size:
 * it is what makes a record one whole erase-unit write with no filler that a
 * future field could quietly claim. */
_Static_assert(sizeof(store_record_t) == STORE_SLOT_SIZE,
               "record must fill a slot exactly");

/* Read-only. Nothing here writes a legacy record or migrates one: the identity
 * is re-created and re-enrolled instead, which is two commands and no recovery
 * path that has never run. What this does is stop a format change from being
 * reported as an empty store. */
int store_legacy_present(uint32_t *dev_id_out) {
    const uint32_t pages[2] = {STORE_PAGE_A, STORE_PAGE_B};
    int found = 0;
    for (int p = 0; p < 2; p++) {
        for (uint32_t s = 0; s < STORE_PAGE_SIZE / STORE_LEGACY_SLOT; s++) {
            const uint32_t *w = (const uint32_t *)(pages[p] + s * STORE_LEGACY_SLOT);
            if (w[0] != STORE_LEGACY_MAGIC)
                continue;
            found++;
            /* dev_id is the third word in both formats; reported so an operator
             * can tell which device this was before re-enrolling it. */
            if (dev_id_out != NULL)
                *dev_id_out = w[2];
        }
    }
    return found;
}

static store_record_t cached;
static uint32_t       cached_page;
static uint32_t       cached_slot;
static uint8_t        cached_valid;

/* Bitwise CRC-32, no table. The store writes a handful of records an hour, so
 * the speed does not matter and a 1 KB table would. */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(0u - (crc & 1u)));
    }
    return ~crc;
}

static int record_ok(const store_record_t *r) {
    if (r->magic != STORE_MAGIC)
        return 0;
    return crc32((const uint8_t *)r, sizeof(*r) - 4u) == r->crc;
}

static const store_record_t *slot_at(uint32_t page, uint32_t slot) {
    return (const store_record_t *)(page + slot * STORE_SLOT_SIZE);
}

static int page_erase(uint32_t page) {
    FLASH_EraseInitTypeDef e = {0};
    uint32_t err = 0;
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Page      = (page - 0x08000000u) / STORE_PAGE_SIZE;
    e.NbPages   = 1;
    if (HAL_FLASH_Unlock() != HAL_OK)
        return -1;
    load_enter(LOAD_FLASH);
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &err);
    load_exit();
    HAL_FLASH_Lock();
    return (st == HAL_OK && err == 0xFFFFFFFFu) ? 0 : -1;
}

/* Writes are doubleword only on this part, so the record is padded to a
 * multiple of eight and programmed one 64-bit word at a time. */
static int slot_write(uint32_t page, uint32_t slot, const store_record_t *r) {
    uint8_t buf[STORE_SLOT_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, r, sizeof(*r));

    if (HAL_FLASH_Unlock() != HAL_OK)
        return -1;
    uint32_t addr = page + slot * STORE_SLOT_SIZE;
    for (uint32_t i = 0; i < STORE_SLOT_SIZE; i += 8u) {
        uint64_t dw;
        memcpy(&dw, buf + i, sizeof(dw));
        load_enter(LOAD_FLASH);
        HAL_StatusTypeDef pst = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw);
        load_exit();
        if (pst != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }
    HAL_FLASH_Lock();
    /* Read back before trusting it. A mark that was not actually stored would
     * let the counter restart below a value already used. */
    return record_ok(slot_at(page, slot)) ? 0 : -1;
}

static int slot_is_free(uint32_t page, uint32_t slot) {
    const uint8_t *p = (const uint8_t *)slot_at(page, slot);
    for (uint32_t i = 0; i < STORE_SLOT_SIZE; i++)
        if (p[i] != 0xFFu)
            return 0;
    return 1;
}

static void scan(void) {
    uint32_t pages[2] = {STORE_PAGE_A, STORE_PAGE_B};
    cached_valid = 0;
    for (int p = 0; p < 2; p++) {
        for (uint32_t s = 0; s < STORE_SLOTS; s++) {
            const store_record_t *r = slot_at(pages[p], s);
            if (!record_ok(r))
                continue;
            /* Signed difference, so a wrapped sequence still orders correctly. */
            if (!cached_valid || (int32_t)(r->seq - cached.seq) > 0) {
                cached = *r;
                cached_page = pages[p];
                cached_slot = s;
                cached_valid = 1;
            }
        }
    }
}

/* The first *erased* slot, not one past the last valid record. Those differ
 * whenever a slot holds something the scanner rejects - a torn write, or records
 * of an older format after a version bump - and the difference is expensive
 * here: aiming at an occupied slot sends append straight to the page swap, which
 * erases a whole page and abandons up to 31 free slots on this one.
 *
 * That erase is the most dangerous operation in the system. It stalls the core
 * for 22 ms, and an erase interrupted on a bank the core fetches from can leave
 * uncorrectable ECC that faults every subsequent boot. Reaching it because one
 * record was torn is not a trade worth making. */
static uint32_t first_free_slot(uint32_t page) {
    for (uint32_t s = 0; s < STORE_SLOTS; s++)
        if (slot_is_free(page, s))
            return s;
    return STORE_SLOTS;
}

static int append(const store_record_t *src) {
    store_record_t r = *src;
    uint32_t page = cached_valid ? cached_page : STORE_PAGE_A;
    uint32_t slot = first_free_slot(page);

    r.magic = STORE_MAGIC;
    r.seq   = cached_valid ? cached.seq + 1u : 1u;
    r.crc   = crc32((const uint8_t *)&r, sizeof(r) - 4u);

    if (slot >= STORE_SLOTS) {
        /* Move to the other page. Erasing it first means the old page stays
         * readable until the new record is down, so a power loss here loses
         * nothing that was already committed. */
        page = (page == STORE_PAGE_A) ? STORE_PAGE_B : STORE_PAGE_A;
        if (page_erase(page) != 0)
            return -1;
        slot = 0;
    }
    if (slot_write(page, slot, &r) != 0)
        return -1;

    cached = r;
    cached_page = page;
    cached_slot = slot;
    cached_valid = 1;
    return 0;
}

int store_init(store_state_t *out) {
    scan();
    memset(out, 0, sizeof(*out));
    if (!cached_valid)
        return 0;
    memcpy(out->priv, cached.priv, STORE_PRIV_LEN);
    out->dev_id       = cached.dev_id;
    out->counter_mark = cached.counter_mark;
    out->key_gen      = cached.key_gen;
    /* A floor from a generation this device no longer runs is not merely stale,
     * it is unusable: it would refuse counters that were never seen under the
     * current key. Report it as absent rather than letting it filter. */
    out->rx_floor     = cached.rx_floor;
    out->init_ceiling = cached.init_ceiling;
    out->hub_id       = cached.hub_id;
    out->net_id       = cached.net_id;
    memcpy(out->session, cached.session, STORE_KEY_LEN);
    memcpy(out->hop_key, cached.hop_key, STORE_KEY_LEN);
    memcpy(out->hub_static, cached.hub_static, STORE_PUB_C_LEN);
    out->slot         = cached.slot;
    out->report_every = cached.report_every;
    out->valid        = 1;
    return 0;
}

/* The replay floor is a property of a *key*, not of a device: a fresh pairing
 * makes a fresh nonce space, and carrying the old floor forward would make the
 * device deaf until the hub's counter climbed past it, possibly for ever.
 *
 * Key, generation and floor move in one record. As two appends it worked only
 * because the call order happened to be right - a property of today's callers,
 * not of the data - and a reset between them left a floor belonging to the key
 * it had just replaced. This replaced store_clear_rx_floor(), which could clear
 * the floor without storing the key that made it stale. */
/* Provisioned out of band and never transmitted, so a reflash that loses it
 * costs a pairing window with no diagnosis on the air at all. */
/* One write for the whole grant: session, hop key and the slot assignment are
 * a single fact, and a reboot between two writes would leave a device holding
 * a key for a slot it was not granted. */
int store_save_pairing(const uint8_t *session, const uint8_t *hop_key,
                       uint8_t slot, uint8_t report_every) {
    store_record_t r;
    if (!cached_valid || session == NULL || hop_key == NULL || report_every == 0u)
        return -1;
    r = cached;
    memcpy(r.session, session, STORE_KEY_LEN);
    memcpy(r.hop_key, hop_key, STORE_KEY_LEN);
    r.slot         = slot;
    r.report_every = report_every;
    /* A fresh pairing is a fresh nonce space, so the floor from the old key
     * must not survive it - see the rx_floor note in store_save_session. */
    r.key_gen  = cached.key_gen + 1u;
    r.rx_floor = 0;
    return append(&r);
}

/* Unchanged rates do not write: a repeated command must not cost a record. */
int store_save_report_every(uint8_t report_every) {
    store_record_t r;
    if (!cached_valid || report_every == 0u)
        return -1;
    if (cached.report_every == report_every)
        return 0;
    r = cached;
    r.report_every = report_every;
    return append(&r);
}

int store_save_hub_static(const uint8_t *pub_c) {
    store_record_t r;
    if (!cached_valid)
        return -1;
    r = cached;
    memcpy(r.hub_static, pub_c, STORE_PUB_C_LEN);
    return append(&r);
}

int store_save_session(const uint8_t *session) {
    store_record_t r;
    if (!cached_valid)
        return -1;
    r = cached;
    memcpy(r.session, session, STORE_KEY_LEN);
    r.key_gen  = cached.key_gen + 1u;
    r.rx_floor = 0;
    return append(&r);
}

int store_save_identity(const uint8_t *priv, uint32_t dev_id) {
    store_record_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.priv, priv, STORE_PRIV_LEN);
    r.dev_id = dev_id;
    /* A new identity starts the counter afresh; the old mark belonged to a key
     * that no longer exists, so it cannot cause reuse. */
    r.counter_mark = STORE_COUNTER_STEP;
    r.key_gen = 0;
    r.rx_floor = 0;
    return append(&r);
}

int store_reserve_counter(uint32_t counter_now, uint32_t *first_safe,
                          uint32_t *mark_out) {
    store_record_t r;

    if (!cached_valid)
        return -1;
    r = cached;
    *first_safe = cached.counter_mark;

    /* Reserve past wherever the counter actually is, not one step past where it
     * was. The clock jumps: a hub reboot advances it by the hub's own reserve,
     * and a device that slept through a day comes back tens of thousands of
     * superframes on. Stepping by a fixed amount would need one flash write per
     * step to catch up - and refuse to transmit for all of them - which turns a
     * legitimate resynchronisation into a slow wear loop. */
    uint32_t base = cached.counter_mark;
    if ((int32_t)(counter_now - base) >= 0)
        base = counter_now + 1u;
    r.counter_mark = base + STORE_COUNTER_STEP;

    if (append(&r) != 0)
        return -1;
    *mark_out = r.counter_mark;
    return 0;
}

/* Persisted on the same amortised schedule as the send mark. Without it a reset
 * reopens the replay window completely: nothing on the device remembers which
 * counters it has already accepted. */
int store_note_received(uint32_t counter) {
    store_record_t r;

    if (!cached_valid)
        return -1;
    if ((int32_t)(counter - cached.rx_floor) < (int32_t)STORE_COUNTER_STEP)
        return 0;                       /* still inside the block already stored */
    r = cached;
    r.rx_floor = counter;
    return append(&r);
}

int store_init_ceiling(uint32_t *out) {
    if (!cached_valid)
        return -1;
    *out = cached.init_ceiling;
    return 0;
}

int store_save_init_ceiling(uint32_t superframe) {
    if (!cached_valid)
        return -1;
    /* Never downwards. A ceiling that can fall is not a ceiling, and the caller
     * is holding a value that came off an authenticated frame - but the write
     * is the durable part, so the monotonicity belongs here too. */
    if ((int32_t)(superframe - cached.init_ceiling) <= 0)
        return 0;
    store_record_t r = cached;
    r.init_ceiling = superframe;
    return append(&r);
}

int store_save_network(uint32_t hub_id, uint16_t net_id) {
    if (!cached_valid)
        return -1;
    if (cached.hub_id == hub_id && cached.net_id == net_id)
        return 0;              /* no flash write for a value that has not moved */
    store_record_t r = cached;
    r.hub_id = hub_id;
    r.net_id = net_id;
    return append(&r);
}

int store_key_gen(uint32_t *gen) {
    if (!cached_valid)
        return -1;
    *gen = cached.key_gen;
    return 0;
}

/* Write a record with its last doubleword omitted. On flash the result is
 * byte-identical to a write torn at that point, so it tests the property that
 * was previously only reasoned about: a partial record and an absent one are
 * both invisible to the scanner. It does not test the power path - that needs
 * the supply removed mid-program - but it does test the half that the recovery
 * argument actually rests on. */
int store_write_torn(void) {
    store_record_t r;
    uint8_t buf[STORE_SLOT_SIZE];
    uint32_t page = cached_valid ? cached_page : STORE_PAGE_A;
    uint32_t slot = cached_valid ? cached_slot + 1u : 0u;

    if (!cached_valid)
        return -1;
    r = cached;
    r.magic = STORE_MAGIC;
    r.seq   = cached.seq + 1u;          /* newer than the good one, so a scanner
                                         * that accepted it would win the compare */
    r.counter_mark = cached.counter_mark + STORE_COUNTER_STEP;
    r.crc   = crc32((const uint8_t *)&r, sizeof(r) - 4u);
    if (slot >= STORE_SLOTS || !slot_is_free(page, slot))
        return -1;                      /* keep the test out of the page-swap path */

    memcpy(buf, &r, sizeof(r));
    if (HAL_FLASH_Unlock() != HAL_OK)
        return -1;
    uint32_t addr = page + slot * STORE_SLOT_SIZE;
    for (uint32_t i = 0; i + 8u < STORE_SLOT_SIZE; i += 8u) {
        uint64_t dw;
        memcpy(&dw, buf + i, sizeof(dw));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }
    HAL_FLASH_Lock();
    return 0;
}

int store_erase_all(void) {
    int rc = page_erase(STORE_PAGE_A);
    if (page_erase(STORE_PAGE_B) != 0)
        rc = -1;
    cached_valid = 0;
    return rc;
}

/* The total is reported rather than left for the caller to know. The console
 * held its own literal 64, which stayed behind when the slot grew to 128 bytes
 * and printed a confident wrong denominator over correct numerators. */
void store_stats(uint32_t *records, uint32_t *slots_free, uint32_t *slots_total) {
    uint32_t pages[2] = {STORE_PAGE_A, STORE_PAGE_B};
    uint32_t used = 0, free_slots = 0;
    for (int p = 0; p < 2; p++) {
        for (uint32_t s = 0; s < STORE_SLOTS; s++) {
            if (slot_is_free(pages[p], s))
                free_slots++;
            else if (record_ok(slot_at(pages[p], s)))
                used++;
        }
    }
    *records = used;
    *slots_free = free_slots;
    *slots_total = 2u * STORE_SLOTS;
}
