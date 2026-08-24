cmake_minimum_required(VERSION 3.22)

# Generated once by STM32CubeMX and hand-maintained since.

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME wl55_device)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# CubeMX bakes absolute paths in; override with -DCUBE_FW_PATH=<path>.
if(NOT DEFINED CUBE_FW_PATH)
    if(DEFINED ENV{CUBE_FW_PATH})
        set(CUBE_FW_PATH "$ENV{CUBE_FW_PATH}")
    else()
        file(GLOB _fw_candidates "$ENV{HOME}/STM32Cube/Repository/STM32Cube_FW_WL_V*")
        list(SORT _fw_candidates)
        list(REVERSE _fw_candidates)
        list(GET _fw_candidates 0 CUBE_FW_PATH)
    endif()
endif()
if(NOT EXISTS "${CUBE_FW_PATH}")
    message(FATAL_ERROR "Cube FW package not found. Pass -DCUBE_FW_PATH=<path>.")
endif()
message(STATUS "Cube FW: ${CUBE_FW_PATH}")

# Off is the product build; the protocol does not change either way.
# radio_devices_docs/wl55_device/testing/console.md
option(WL55_CONSOLE "compile the read-only console" ON)

# On adds commands that change the node. Off is the product. ROADMAP 77
option(WL55_DEV_COMMANDS "console commands that change what the node does" OFF)

# NODE is the product build; HUB drives the same air from a second board.
# radio_devices_docs/radio/phy-seam.md
set(WL55_ROLE "NODE" CACHE STRING "NODE or HUB")
set_property(CACHE WL55_ROLE PROPERTY STRINGS NODE HUB)
if(NOT WL55_ROLE STREQUAL "NODE" AND NOT WL55_ROLE STREQUAL "HUB")
    message(FATAL_ERROR "WL55_ROLE must be NODE or HUB, not ${WL55_ROLE}")
endif()

# The device this hub build invites, and the service gap it injects each pass.
# radio_devices_docs/radio/phy-seam.md
set(WL55_HUB_DEV_ID "0x751C5A3Bu" CACHE STRING "the device a HUB build invites")
set(WL55_HUB_ID "0x574C3535u" CACHE STRING "the network id a HUB build claims")
set(WL55_HUB_STALL_US "0" CACHE STRING "microseconds of injected service gap")

# The wire vectors are included from the hub's tree, never copied.
# radio_devices_docs/wl55_device/testing/host-tests.md
if(NOT DEFINED OPENHUB_PATH)
    if(DEFINED ENV{OPENHUB_PATH})
        set(OPENHUB_PATH "$ENV{OPENHUB_PATH}")
    else()
        set(OPENHUB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../OpenHub")
    endif()
endif()
set(WIRE_VECTORS "${OPENHUB_PATH}/Common/test/vectors")
set(SHARED_PROTOCOL "${OPENHUB_PATH}/Common/inc")
# Probed on a set this build includes; it was probing a retired one.
# radio_devices_docs/wl55_device/testing/host-tests.md
if(NOT EXISTS "${WIRE_VECTORS}/wire_v4.h")
    message(FATAL_ERROR
        "Wire vectors not found at ${WIRE_VECTORS}. Pass -DOPENHUB_PATH=<hub repo>.")
endif()
message(STATUS "Wire vectors: ${WIRE_VECTORS}")

set(MX_GENERATED_IN  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32cubemx/CMakeLists.txt")
set(MX_GENERATED_OUT "${CMAKE_CURRENT_BINARY_DIR}/mx-stm32cubemx/CMakeLists.txt")
file(READ "${MX_GENERATED_IN}" MX_GENERATED_TEXT)
string(REGEX REPLACE
    "/[^ \t\r\n]*STM32Cube/Repository/STM32Cube_FW_WL_V[0-9.]+/"
    "${CUBE_FW_PATH}/" MX_GENERATED_TEXT "${MX_GENERATED_TEXT}")
# The relative path moves once the processed copy lives in the build tree.
string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_SOURCE_DIR}"
       MX_GENERATED_TEXT "${MX_GENERATED_TEXT}")
file(WRITE "${MX_GENERATED_OUT}" "${MX_GENERATED_TEXT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MX_GENERATED_IN}")

# Add STM32CubeMX generated sources
add_subdirectory("${CMAKE_CURRENT_BINARY_DIR}/mx-stm32cubemx" mx-stm32cubemx-build)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# What is on the board must be answerable from the board. ROADMAP item 48
set(BUILD_ID_H "${CMAKE_CURRENT_BINARY_DIR}/generated/build_id.h")
add_custom_target(build_id ALL
    BYPRODUCTS "${BUILD_ID_H}"
    COMMAND ${CMAKE_COMMAND} -DSRC="${CMAKE_CURRENT_SOURCE_DIR}"
            -DOUT="${BUILD_ID_H}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake"
    COMMENT "Resolving build id from git")
add_dependencies(${CMAKE_PROJECT_NAME} build_id)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/timebase.c
    Core/Src/uptime.c
    Core/Src/telemetry.c
    Core/Src/device.c
    Core/Src/console.c
    Core/Src/vcp.c
    Core/Src/radio.c
    Core/Src/crypto.c
    Core/Src/sha256.c
    Core/Src/frame.c
    Core/Src/pairing.c
    Core/Src/exchange.c
    Core/Src/vectors.c
    Core/Src/join.c
    Core/Src/store.c
    Core/Src/hop.c
    Core/Src/hop_prf.c
    Core/Src/phy_sx126x.c
    Core/Src/hublogic.c
    Core/Src/load.c
    Core/Src/pair_init.c
    Core/Src/sensor.c
    third_party/monocypher/monocypher.c
    # The RF switch is BSP territory: the .ioc pulls in the nucleo half only.
    ${CUBE_FW_PATH}/Drivers/BSP/STM32WLxx_Nucleo/stm32wlxx_nucleo_radio.c
)

# The link layer, one set of sources; deleting them there fails this build.
# radio_devices_docs/specs/03-roadmap.md
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${OPENHUB_PATH}/Common/src/grid.c
    ${OPENHUB_PATH}/Common/src/gridmaster.c
    ${OPENHUB_PATH}/Common/src/superframe.c
    ${OPENHUB_PATH}/Common/src/beacon.c
)

# The hub's hop.c under renamed symbols, dev builds only. ADR-0029 decision 5.
# radio_devices_docs/radio/hopping.md
if(WL55_DEV_COMMANDS)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/hopref.c)
    set_source_files_properties(Core/Src/hopref.c PROPERTIES
        COMPILE_OPTIONS "-iquote${SHARED_PROTOCOL}"
        COMPILE_DEFINITIONS
            "OPENHUB_HOP_H=\"${SHARED_PROTOCOL}/hop.h\";OPENHUB_HOP_C=\"${OPENHUB_PATH}/Common/src/hop.c\"")
endif()

# -O0 costs this file 371 ms a scalar multiplication against the PKA's 103 ms.
# radio_devices_docs/radio/decisions/0025-x25519-replaces-p256.md
set_source_files_properties(third_party/monocypher/monocypher.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Core/Inc
    third_party/monocypher
    ${WIRE_VECTORS}
    ${SHARED_PROTOCOL}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    WL55_CONSOLE=$<BOOL:${WL55_CONSOLE}>
    WL55_DEV_COMMANDS=$<BOOL:${WL55_DEV_COMMANDS}>
    WL55_ROLE_HUB=$<STREQUAL:${WL55_ROLE},HUB>
    WL55_HUB_DEV_ID=${WL55_HUB_DEV_ID}
    WL55_HUB_ID=${WL55_HUB_ID}
    WL55_HUB_STALL_US=${WL55_HUB_STALL_US}u
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
