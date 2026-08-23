/* The link vectors had no host consumer, so a moved wire reached the board as a warning.
 * radio_devices_docs/wl55_device/testing/host-tests.md */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "radio_protocol.h"
#include "link_v6.h"

/* A size assert answers "same shape", never "same contract": v4 built clean on v5. */
_Static_assert(LINK_VECTORS_VERSION == RADIO_LINK_VERSION,
               "the link vectors are not the wire this build speaks");

static int failures;

static void same(const char *what, const void *got, const void *want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        const uint8_t *g = got, *w = want;
        printf("FAIL %-42s\n     got ", what);
        for (size_t i = 0; i < len; i++)
            printf("%02x%s", g[i], g[i] != w[i] ? "*" : " ");
        printf("\n    want ");
        for (size_t i = 0; i < len; i++)
            printf("%02x ", w[i]);
        printf("\n");
        failures++;
    } else {
        printf("  %-42s ok\n", what);
    }
}

int main(void) {
    /* Laid out, never sealed: order and offset are what move inside a fixed width.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    radio_uplink_report_t rep = { -92, 0x05, 3287, 61, 0x5b, 0x01, 0x1f, 0x02,
                                  -173, {0xa1, 0xb2} };
    radio_downlink_cmd_t cmd = { RADIO_CMD_SET_RATE, 12, 0x1234u, 0x00112233u, 0x5b, 6,
                                 {0x11, 0x22, 0x33, 0x44, 0x55, 0x66} };

    printf("  link_v%u %s\n", LINK_VECTORS_VERSION, LINK_VECTORS_DIGEST);
    same("uplink report layout vs LV_UPLINK_PLAIN", &rep, LV_UPLINK_PLAIN, sizeof(rep));
    same("downlink cmd layout vs LV_DOWNLINK_PLAIN", &cmd, LV_DOWNLINK_PLAIN, sizeof(cmd));
    /* The vector's ack_arg is 31 and its report_every 12, so echoing the command fails. */
    same("ack_arg is not the commanded rate", &(uint8_t){LV_UPLINK_PLAIN[10]},
         &(uint8_t){0x1fu}, 1u);

    if (failures) {
        printf("\n%d link check(s) failed\n", failures);
        return 1;
    }
    printf("\nall link checks passed\n");
    return 0;
}
