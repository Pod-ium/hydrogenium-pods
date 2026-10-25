# pd_drivers: chip drivers for RP2040/RP2350 (device builds only).
# Each driver takes its pins in a Config struct, so any terminal can reuse it.
add_library(pd_drivers INTERFACE)
target_sources(pd_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/st7365p.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_spi.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/picocalc_southbridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/pwm_audio.cpp)
target_include_directories(pd_drivers INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_drivers INTERFACE pd_hal pd_core
    pico_stdlib pico_sync hardware_spi hardware_i2c hardware_dma hardware_pwm hardware_gpio)
