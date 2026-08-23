/* Identity and superframe mark in the last two flash pages, append-only.
 * radio_devices_docs/wl55_device/arch/store.md */
#include <string.h>

#include "main.h"
#include "store.h"
#include "load.h"

#define STORE_PAGE_A     0x0803F000u
#define STORE_PAGE_B     0x0803F800u
#define STORE_PAGE_SIZE  2048u
#define STORE_SLOT_SIZE  256u
#define STORE_SLOTS      (STORE_PAGE_SIZE / STORE_SLOT_SIZE)
/* The 256-byte record. Older formats fail this and the scanner steps over them.
 * radio_devices_docs/wl55_device/arch/store.md */
#define STORE_MAGIC      0x4B535735u
/* The 128-byte format: unreadable here is a different fact from absent.
 * radio_devices_docs/wl55_device/arch/store.md */
#define STORE_LEGACY_MAGIC  0x4B535734u
#define STORE_LEGACY_SLOT   128u

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t dev_id;
    uint32_t counter_mark;   /* retired with the reservation; ADR-0023 */
    uint8_t  priv[STORE_PRIV_LEN];
    uint32_t key_gen;       /**< which generation rx_floor belongs to, so a stale one shows */
    uint32_t rx_floor;      /* replay window survives a reset only if stored */
    uint8_t  session[STORE_KEY_LEN];        /**< the grant; the four below have no writer yet */
    uint8_t  hop_key[STORE_KEY_LEN];        /* network-wide, from PAIR_ACCEPT */
    uint8_t  hub_static[STORE_PUB_C_LEN];   /* learned at pairing - ADR-0024 */
    uint8_t  slot;                          /* granted uplink slot */
    uint8_t  report_every;
    uint32_t init_ceiling;  /**< durable, because a rate limit resets on reboot */
    uint32_t hub_id;        /**< carved out of the pad, so 0 still means absent */
    uint16_t net_id;
    uint8_t  pad[120];      /* fills the slot exactly; covered by the CRC */
    uint32_t crc;
} __attribute__((packed)) store_record_t;

/* Filling the slot exactly is the invariant: one erase-unit write, no spare filler.
 * radio_devices_docs/wl55_device/arch/store.md */
_Static_assert(sizeof(store_record_t) == STORE_SLOT_SIZE,
               "record must fill a slot exactly");

/* Read-only. Nothing migrates: the identity is re-created and re-enrolled.
 * radio_devices_docs/wl55_device/arch/store.md */
int store_legacy_present(uint32_t *dev_id_out) {
    const uint32_t pages[2] = {STORE_PAGE_A, STORE_PAGE_B};
    int found = 0;
    for (int p = 0; p < 2; p++) {
        for (uint32_t s = 0; s < STORE_PAGE_SIZE / STORE_LEGACY_SLOT; s++) {
            const uint32_t *w = (const uint32_t *)(pages[p] + s * STORE_LEGACY_SLOT);
            if (w[0] != STORE_LEGACY_MAGIC)
                continue;
            found++;
            /* dev_id is the third word in both formats, so the operator can name the device.
             * radio_devices_docs/wl55_device/arch/store.md */
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

/* Bitwise, no table: a handful of records an hour, and 1 KB would matter.
 * radio_devices_docs/wl55_device/arch/store.md */
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

/* Doubleword writes only on this part, so the record is padded to a multiple of 8.
 * radio_devices_docs/wl55_device/arch/store.md */
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
    /* Read back before trusting it: an unstored mark restarts below a used counter.
     * radio_devices_docs/wl55_device/arch/store.md */
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

/* The first erased slot, never one past the last valid record: the page swap is 22 ms.
 * radio_devices_docs/wl55_device/arch/store.md */
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
        /* Erase the destination first: the old page stays readable until the record is down.
         * radio_devices_docs/wl55_device/arch/store.md */
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
    /* Absent, not filtering: a floor from another generation refuses unseen counters.
     * radio_devices_docs/wl55_device/arch/store.md */
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

/* One write for the whole grant: session, hop key and slot are a single fact.
 * radio_devices_docs/wl55_device/arch/store.md */
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
    /* A fresh pairing is a fresh nonce space, so the old key's floor must not survive.
     * radio_devices_docs/wl55_device/arch/store.md */
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

int store_hub_static_set(const store_state_t *st) {
    for (uint32_t i = 0; i < STORE_PUB_C_LEN; i++)
        if (st->hub_static[i] != 0u)
            return 1;
    return 0;
}

int store_save_hub_static(const uint8_t *pub_c) {
    store_record_t r;
    if (!cached_valid)
        return -1;
    r = cached;
    memcpy(r.hub_static, pub_c, STORE_PUB_C_LEN);
    return append(&r);
}

/* Key and floor in one record: hub_eph is gone, so there is no second chance.
 * radio_devices_docs/wl55_device/arch/store.md */
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
    /* A new identity starts the counter afresh; the old mark's key no longer exists.
     * radio_devices_docs/wl55_device/arch/store.md */
    r.counter_mark = STORE_COUNTER_STEP;
    r.key_gen = 0;
    r.rx_floor = 0;
    return append(&r);
}


/* Amortised with the send mark: without it a reset reopens the replay window.
 * radio_devices_docs/wl55_device/arch/store.md */
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

int store_save_init_ceiling(uint32_t superframe) {
    if (!cached_valid)
        return -1;
    /* Never downwards, and here rather than at the caller: the write is the durable part.
     * radio_devices_docs/wl55_device/arch/store.md */
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

/* The identity is what a label names, so a release must not draw a new one.
 * radio_devices_docs/wl55_device/arch/store.md */
int store_release_pairing(void) {
    store_record_t r;

    if (!cached_valid)
        return -1;
    r = cached;
    memset(r.session,    0, STORE_KEY_LEN);
    memset(r.hop_key,    0, STORE_KEY_LEN);
    memset(r.hub_static, 0, STORE_PUB_C_LEN);
    r.slot         = 0;
    r.report_every = 0;          /* what join_restore() reads as "not paired" */
    /* The binding to the old hub, or the node refuses its replacement WRONG_NET. */
    r.hub_id = 0;
    r.net_id = 0;
    /* A position in the old hub's timeline; the window bounds a replay now. */
    r.init_ceiling = 0;
    /* A fresh nonce space, the same rule store_save_pairing() follows. */
    r.key_gen  = cached.key_gen + 1u;
    r.rx_floor = 0;
    return append(&r);
}

int store_key_gen(uint32_t *gen) {
    if (!cached_valid)
        return -1;
    *gen = cached.key_gen;
    return 0;
}

/* A record short one doubleword is byte-identical to a torn write. Not the power path.
 * radio_devices_docs/wl55_device/arch/store.md */
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

/* The store reports its own total: the console's literal stayed at 64 once.
 * radio_devices_docs/wl55_device/arch/store.md */
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
