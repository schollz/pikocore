cmake_minimum_required(VERSION 3.18)

set(PICO_SDK_PATH "../pico-sdk")

include(pico_sdk_import.cmake)

project(pikocore C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(${PROJECT_NAME} main.cpp ${CMAKE_CURRENT_LIST_DIR}/doth/WS2812.cpp)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/doth/WS2812.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/doth/onewiremidi.pio)

target_link_libraries(${PROJECT_NAME} 
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

set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,--print-memory-usage")

pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)


include(target_compile_definitions.cmake)

pico_add_extra_outputs(${PROJECT_NAME})