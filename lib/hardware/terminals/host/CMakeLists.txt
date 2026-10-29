# pd_terminal_host: PC terminal (framebuffer + PPM dumps, scripted keys).
add_library(pd_terminal_host INTERFACE)
target_sources(pd_terminal_host INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/host.cpp)
target_include_directories(pd_terminal_host INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_terminal_host INTERFACE pd_terminal_api pd_fs pd_input pd_core)

# pd_host_web: the browser front end (emcmake builds). picopods_add_pod()
# links it into pods, which then run in web/run.html.
if(EMSCRIPTEN)
    add_library(pd_host_web INTERFACE)
    target_sources(pd_host_web INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/host_web.cpp)
    target_link_libraries(pd_host_web INTERFACE pd_terminal_host)
endif()

# pd_host_window: the interactive SDL2 window. picopods_add_pod() links it
# into pods only (it brings its own main(); tests keep theirs).
set(PD_HOST_WINDOW OFF CACHE INTERNAL "")
if(EMSCRIPTEN)
    # the browser front end replaces it
elseif(NOT PD_HOST_SDL STREQUAL "OFF")
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2)
        add_library(pd_host_window INTERFACE)
        target_sources(pd_host_window INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/host_window.cpp)
        target_link_libraries(pd_host_window INTERFACE pd_terminal_host SDL2::SDL2)
        set(PD_HOST_WINDOW ON CACHE INTERNAL "")
        message(STATUS "PicoPods: host window (SDL2 ${SDL2_VERSION})")
    elseif(PD_HOST_SDL STREQUAL "ON")
        message(FATAL_ERROR "PicoPods: PD_HOST_SDL=ON but SDL2 was not found (install libsdl2-dev)")
    else()
        message(STATUS "PicoPods: SDL2 not found, pods run headless only")
    endif()
endif()
