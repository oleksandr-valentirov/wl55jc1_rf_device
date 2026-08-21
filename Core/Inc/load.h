#pragma once

#include <stdint.h>

/* Where the CPU's time actually goes.
 *
 * The distinction that matters is not busy against idle but *recoverable*
 * against not: cycles spent polling a radio interrupt can be given back by an
 * interrupt or a sleep, cycles spent inside AES cannot. A single "load" number
 * hides exactly that. */
typedef enum {
    LOAD_CRYPTO = 0,    /* AES and SHA - real computation, not recoverable */
    LOAD_PKA,           /* P-256 - the accelerator runs, the core only waits */
    LOAD_RADIO_WAIT,    /* polling the radio's IRQ line, recoverable */
    LOAD_RADIO_SPI,     /* SUBGHZSPI traffic, including the polling reads */
    LOAD_FLASH,         /* erase and program, which stall the core */
    LOAD_CONSOLE,       /* blocking UART, a test-rig cost and not a protocol one */
    LOAD_CATEGORIES
} load_cat_t;

/** @brief Starts a new measurement window. */
void        load_reset(void);

/** @brief Enters a category; nesting is not supported by design. */
void        load_enter(load_cat_t cat);

/** @brief Leaves the current category and charges it the elapsed time. */
void        load_exit(void);

/** @brief The window's length, the denominator for every category. */
uint32_t    load_window_us(void);

/** @brief Microseconds charged to a category this window. */
uint32_t    load_us(load_cat_t cat);

/** @brief Times a category was entered this window. */
uint32_t    load_calls(load_cat_t cat);

/** @brief The longest single visit to a category; a mean hides a stall. */
uint32_t    load_max_us(load_cat_t cat);

/** @brief Names a category for the console. */
const char *load_name(load_cat_t cat);
