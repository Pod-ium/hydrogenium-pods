# pd_core: types, time, clocks, second core, board capabilities.
add_library(pd_core INTERFACE)
target_include_directories(pd_core INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
if(PD_HOST)
    target_sources(pd_core INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/sys_host.cpp)
    target_compile_definitions(pd_core INTERFACE PD_HOST=1)
    find_package(Threads REQUIRED)
    target_link_libraries(pd_core INTERFACE Threads::Threads)
else()
    target_sources(pd_core INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/sys_rp2.cpp)
    target_link_libraries(pd_core INTERFACE pico_stdlib pico_multicore pico_flash
        hardware_clocks hardware_vreg hardware_watchdog)
endif()
