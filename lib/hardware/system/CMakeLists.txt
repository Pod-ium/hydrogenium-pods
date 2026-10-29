# pd_flash_cache: file -> memory-mapped flash (ROMs, big tables).
add_library(pd_flash_cache INTERFACE)
target_include_directories(pd_flash_cache INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_flash_cache INTERFACE pd_fs pd_core)
if(PD_HOST)
    target_sources(pd_flash_cache INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/flash_cache_host.cpp)
else()
    target_sources(pd_flash_cache INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/flash_cache_rp2.cpp)
    target_link_libraries(pd_flash_cache INTERFACE pico_flash hardware_flash)
endif()

# pd_diag: start-up log, reset breadcrumb, crash screen (installs the
# HardFault handler and the panic hook).
add_library(pd_diag INTERFACE)
target_sources(pd_diag INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/diag.cpp)
target_include_directories(pd_diag INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_diag INTERFACE pd_gfx pd_core)
if(NOT PD_HOST)
    target_compile_definitions(pd_diag INTERFACE PICO_PANIC_FUNCTION=pd_panic)
endif()
