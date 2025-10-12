cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(ubasic_picocalc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2)

pico_sdk_init()

add_executable(ubasic_picocalc
    main.c
    drivers/audio.c
    drivers/display.c
    drivers/keyboard.c
    drivers/lcd.c
    drivers/onboard_led.c
    drivers/picocalc.c
    drivers/sdcard.c
    drivers/fat32.c
    drivers/serial.c
    drivers/southbridge.c
    drivers/font-5x10.c
    drivers/font-8x10.c
    drivers/clib.c

    ubasic/ubasic.c
    ubasic/tokenizer.c
)

pico_generate_pio_header(ubasic_picocalc ${CMAKE_CURRENT_LIST_DIR}/drivers/audio.pio)

target_include_directories(ubasic_picocalc PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/drivers
    ${CMAKE_CURRENT_LIST_DIR}/ubasic
)

target_link_libraries(ubasic_picocalc PRIVATE
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_i2c
    hardware_uart
    pico_multicore
)

pico_enable_stdio_usb(ubasic_picocalc 1)
pico_enable_stdio_uart(ubasic_picocalc 1)

pico_add_extra_outputs(ubasic_picocalc)

