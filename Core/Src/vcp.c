/* Telemetry owns the port; the console shares it only where it is compiled in.
 * radio_devices_docs/wl55_device/testing/console.md */
#include "vcp.h"

#include "load.h"
#include "main.h"

extern UART_HandleTypeDef hcom_uart[];

void vcp_write(const uint8_t *data, uint16_t len, uint32_t timeout) {
    if (len == 0u)
        return;
    /* Charged apart from the protocol: this is the cost of the rig. */
    load_enter(LOAD_CONSOLE);
    HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)data, len, timeout);
    load_exit();
}
