# Writes build_id.h from git, rewriting only on a change so nothing rebuilds
# without cause. Run at build time: a configure-time value goes stale silently.
# radio_devices_docs/wl55_device/testing/telemetry.md

execute_process(COMMAND git -C "${SRC}" describe --always --dirty
                OUTPUT_VARIABLE ID OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE RC)
if(NOT RC EQUAL 0 OR ID STREQUAL "")
    set(ID "unknown")
endif()

# The abbreviated commit as a number, so a telemetry record can carry it.
set(DIRTY 0)
if(ID MATCHES "-dirty$")
    set(DIRTY 1)
endif()
string(REGEX MATCH "[0-9a-f]+" HEX "${ID}")
set(COMMIT 0)
if(NOT HEX STREQUAL "")
    string(SUBSTRING "${HEX}" 0 7 HEX7)
    math(EXPR COMMIT "0x${HEX7}" OUTPUT_FORMAT DECIMAL)
endif()

file(WRITE "${OUT}.tmp"
"/* Generated at build time by cmake/build_id.cmake. Do not edit. */
#pragma once

#define BUILD_ID_STR    \"${ID}\"
#define BUILD_ID_COMMIT ${COMMIT}u
#define BUILD_ID_DIRTY  ${DIRTY}u
")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
