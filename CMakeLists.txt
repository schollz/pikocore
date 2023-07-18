cmake_minimum_required(VERSION 3.18)

set(PICO_SDK_PATH "../pico-sdk")

include(pico_sdk_import.cmake)

project(pikocore C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(pikocore main.cpp ${CMAKE_CURRENT_LIST_DIR}/doth/WS2812.cpp)
pico_generate_pio_header(pikocore ${CMAKE_CURRENT_LIST_DIR}/doth/WS2812.pio)

target_link_libraries(pikocore 
	pico_stdlib
	hardware_adc
	hardware_dma
	hardware_pio
	hardware_timer
	hardware_clocks
	hardware_pwm
	hardware_flash
	hardware_sync
)

pico_enable_stdio_usb(pikocore 1)
pico_enable_stdio_uart(pikocore 0)
pico_add_extra_outputs(pikocore)