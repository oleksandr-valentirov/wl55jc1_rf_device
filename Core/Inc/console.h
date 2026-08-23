#pragma once

#include <stdint.h>

/**
 * @file console.h
 * @brief A read-only window on the node, present only where WL55_CONSOLE is on.
 *
 * The node runs the same protocol whether or not this is compiled in. Nothing
 * here starts, arms or stops anything: every command answers a question.
 *
 * radio_devices_docs/wl55_device/testing/console.md
 */

/** @brief Greets the port and enables receive, or does nothing when compiled out. */
void console_init(void);
/** @brief Answers whatever has been typed since the last pass. */
void console_poll(void);
/** @brief Takes one byte from the UART interrupt. */
void console_rx_byte(uint8_t b);
