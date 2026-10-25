# hydrogenium-pods - the common PicoPods libraries for the PicoCalc, plus the
# example pods (hardware check, pod skeleton, USB port probe).
#
# Device build (default: PicoCalc + Pico 2 W):
#   cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
#   cmake --build build -j
# Other board:    -DPD_BOARD=pico   (pico, pico_w, pico2, pico2_w...)
# PC build (simulator + unit tests):
#   cmake -B build-host -DPD_TERMINAL=host && cmake --build build-host -j
#   ctest --test-dir build-host
#
# The pods themselves live in their own repositories and use this one through
# cmake/picopods_import.cmake: helium-pods (games), carbo-pods (productivity),
# aurum-pods (emulators and PicoPods Web).
cmake_minimum_required(VERSION 3.13)
include(cmake/picopods_init.cmake)
project(picopods C CXX ASM)
include(cmake/picopods.cmake)

# Examples: hardware check / library tour, pod skeleton, USB port probe
add_subdirectory(examples/demo)
add_subdirectory(examples/template)
add_subdirectory(examples/usbprobe)

if(PD_HOST AND NOT EMSCRIPTEN)   # the browser build has pods only
    enable_testing()
    add_subdirectory(tests)
endif()
