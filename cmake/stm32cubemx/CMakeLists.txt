cmake_minimum_required(VERSION 3.22)
# Enable CMake support for ASM and C languages
enable_language(C ASM)
# STM32CubeMX generated symbols (macros)
set(MX_Defines_Syms 
	USE_NUCLEO_64 
	CORE_CM4 
	USE_HAL_DRIVER 
	STM32WL55xx
    $<$<CONFIG:Debug>:DEBUG>
)

# STM32CubeMX generated include paths
set(MX_Include_Dirs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Inc
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Inc
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Inc/Legacy
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/BSP/STM32WLxx_Nucleo
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/CMSIS/Device/ST/STM32WLxx/Include
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/CMSIS/Include
)

# STM32CubeMX generated application sources
set(MX_Application_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/stm32wlxx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/stm32wlxx_hal_msp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/sysmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../startup_stm32wl55xx_cm4.s
)

# STM32 HAL/LL Drivers
set(STM32_Drivers_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/system_stm32wlxx.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_adc.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_adc_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_ll_adc.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rcc.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rcc_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_flash.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_flash_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_gpio.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_dma.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_dma_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_pwr.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_pwr_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_cortex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_exti.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_cryp.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_cryp_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_pka.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rng.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rng_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_subghz.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_tim.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_tim_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_usart.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_usart_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_uart_ex.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_uart.c
    /home/aleks/STM32Cube/Repository/STM32Cube_FW_WL_V1.6.0/Drivers/BSP/STM32WLxx_Nucleo/stm32wlxx_nucleo.c
)

# Drivers Midllewares



# Link directories setup
set(MX_LINK_DIRS

)
# Project static libraries
set(MX_LINK_LIBS 
    STM32_Drivers
    ${TOOLCHAIN_LINK_LIBRARIES}
    
)
# Interface library for includes and symbols
add_library(stm32cubemx INTERFACE)
target_include_directories(stm32cubemx INTERFACE ${MX_Include_Dirs})
target_compile_definitions(stm32cubemx INTERFACE ${MX_Defines_Syms})

# Create STM32_Drivers static library
add_library(STM32_Drivers OBJECT)
target_sources(STM32_Drivers PRIVATE ${STM32_Drivers_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32cubemx)


# Add STM32CubeMX generated application sources to the project
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MX_Application_Src})

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MX_LINK_DIRS})

# Add libraries to the project
target_link_libraries(${CMAKE_PROJECT_NAME} ${MX_LINK_LIBS})

# Add the map file to the list of files to be removed with 'clean' target
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
