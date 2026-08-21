/* Time accounting for a superloop with no scheduler to ask.
 *
 * Nested, and each frame accumulates only its own time: the SPI reads that
 * wait_irq issues while polling are charged to SPI and subtracted from the
 * wait, so "waiting" and "talking to the radio while waiting" do not both
 * claim the same microseconds. */
#include "load.h"
#include "timebase.h"

#define LOAD_STACK_DEPTH 8

typedef struct {
    load_cat_t cat;
    uint32_t   self_us;
} load_frame_t;

static load_frame_t stack[LOAD_STACK_DEPTH];
static uint8_t      depth;
static uint32_t     last_us;
static uint32_t     window_start_us;
static uint32_t     total_us[LOAD_CATEGORIES];
static uint32_t     calls[LOAD_CATEGORIES];
static uint32_t     max_us[LOAD_CATEGORIES];
static uint32_t     overflows;

static const char *const names[LOAD_CATEGORIES] = {
    "crypto", "pka", "radio-wait", "radio-spi", "flash", "console"
};

void load_reset(void) {
    for (int i = 0; i < LOAD_CATEGORIES; i++) {
        total_us[i] = 0;
        calls[i] = 0;
        max_us[i] = 0;
    }
    depth = 0;
    overflows = 0;
    window_start_us = micros();
    last_us = window_start_us;
}

/* Charges everything since the last transition to whatever was running, which
 * is the frame on top - or to nobody, which is what idle means here. */
static void charge(uint32_t now) {
    if (depth > 0)
        stack[depth - 1].self_us += now - last_us;
    last_us = now;
}

void load_enter(load_cat_t cat) {
    uint32_t now = micros();

    if (cat >= LOAD_CATEGORIES)
        return;
    charge(now);
    if (depth >= LOAD_STACK_DEPTH) {
        overflows++;
        return;
    }
    stack[depth].cat = cat;
    stack[depth].self_us = 0;
    depth++;
}

void load_exit(void) {
    uint32_t now = micros();
    load_frame_t *f;

    if (depth == 0)
        return;
    charge(now);
    f = &stack[--depth];
    total_us[f->cat] += f->self_us;
    calls[f->cat]++;
    if (f->self_us > max_us[f->cat])
        max_us[f->cat] = f->self_us;
}

uint32_t load_window_us(void) { return micros() - window_start_us; }
uint32_t load_us(load_cat_t c) { return (c < LOAD_CATEGORIES) ? total_us[c] : 0; }
uint32_t load_calls(load_cat_t c) { return (c < LOAD_CATEGORIES) ? calls[c] : 0; }
uint32_t load_max_us(load_cat_t c) { return (c < LOAD_CATEGORIES) ? max_us[c] : 0; }
const char *load_name(load_cat_t c) { return (c < LOAD_CATEGORIES) ? names[c] : "?"; }
