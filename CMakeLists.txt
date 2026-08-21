cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

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

# STM32CubeMX bakes absolute paths to the Cube FW package into
# cmake/stm32cubemx/CMakeLists.txt. Rewriting the prefix at configure time keeps
# the libraries out of the repository (ProjectManager.LibraryCopy=2) while still
# letting a committed file that carries someone else's paths build here.
# Override with -DCUBE_FW_PATH=<path> or the CUBE_FW_PATH environment variable.
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

# The wire-format vectors live in the hub's tree and are included, not copied:
# two implementations checking the same file is the entire point, and a stale
# copy would break interoperability exactly where the contract is meant to hold.
if(NOT DEFINED OPENHUB_PATH)
    if(DEFINED ENV{OPENHUB_PATH})
        set(OPENHUB_PATH "$ENV{OPENHUB_PATH}")
    else()
        set(OPENHUB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../OpenHub")
    endif()
endif()
set(WIRE_VECTORS "${OPENHUB_PATH}/Common/test/vectors")
set(SHARED_PROTOCOL "${OPENHUB_PATH}/Common/inc")
if(NOT EXISTS "${WIRE_VECTORS}/wire_v1.h")
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
# The generated file reaches the project through ${CMAKE_CURRENT_SOURCE_DIR}/../..,
# which no longer points here once the processed copy lives in the build tree.
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

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/timebase.c
    Core/Src/uptime.c
    Core/Src/telemetry.c
    Core/Src/cli.c
    Core/Src/radio.c
    Core/Src/crypto.c
    Core/Src/sha256.c
    Core/Src/frame.c
    Core/Src/pairing.c
    Core/Src/exchange.c
    Core/Src/vectors.c
    Core/Src/join.c
    Core/Src/store.c
    Core/Src/superframe.c
    Core/Src/beacon.c
    Core/Src/hop.c
    Core/Src/load.c
    Core/Src/pair_init.c
    # The RF switch on this board is BSP territory; the .ioc pulls in
    # stm32wlxx_nucleo.c but not the radio half of the same driver.
    ${CUBE_FW_PATH}/Drivers/BSP/STM32WLxx_Nucleo/stm32wlxx_nucleo_radio.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Core/Inc
    ${WIRE_VECTORS}
    ${SHARED_PROTOCOL}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
