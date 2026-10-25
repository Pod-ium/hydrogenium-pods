# PicoPods - include AFTER project(): initialises the SDK, adds the libraries
# and defines picopods_add_pod().
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT PD_HOST)
    pico_sdk_init()
endif()

if(NOT TARGET pd_core)
    set(_pd ${PICOPODS_ROOT}/lib)
    add_subdirectory(${_pd}/hardware/core       ${CMAKE_BINARY_DIR}/picopods/core)
    add_subdirectory(${_pd}/hardware/hal        ${CMAKE_BINARY_DIR}/picopods/hal)
    add_subdirectory(${_pd}/hardware/input      ${CMAKE_BINARY_DIR}/picopods/input)
    add_subdirectory(${_pd}/services/fs         ${CMAKE_BINARY_DIR}/picopods/fs)
    add_subdirectory(${_pd}/interface/gfx       ${CMAKE_BINARY_DIR}/picopods/gfx)
    add_subdirectory(${_pd}/interface/ui        ${CMAKE_BINARY_DIR}/picopods/ui)
    add_subdirectory(${_pd}/hardware/system     ${CMAKE_BINARY_DIR}/picopods/system)
    add_subdirectory(${_pd}/services/net        ${CMAKE_BINARY_DIR}/picopods/net)
    add_subdirectory(${_pd}/services/crosspoint ${CMAKE_BINARY_DIR}/picopods/crosspoint)
    add_subdirectory(${_pd}/content/text        ${CMAKE_BINARY_DIR}/picopods/text)
    add_subdirectory(${_pd}/content/epub        ${CMAKE_BINARY_DIR}/picopods/epub)
    if(NOT PD_HOST)
        add_subdirectory(${_pd}/hardware/drivers ${CMAKE_BINARY_DIR}/picopods/drivers)
    endif()
    add_subdirectory(${_pd}/hardware/terminals  ${CMAKE_BINARY_DIR}/picopods/terminals)
    if(NOT TARGET pd_terminal_${PD_TERMINAL})
        message(FATAL_ERROR "PicoPods: unknown terminal '${PD_TERMINAL}'")
    endif()
    add_library(pd_terminal INTERFACE)
    target_link_libraries(pd_terminal INTERFACE pd_terminal_${PD_TERMINAL})
    add_subdirectory(${_pd}/interface/app       ${CMAKE_BINARY_DIR}/picopods/app)
    add_subdirectory(${_pd}/hardware/usb        ${CMAKE_BINARY_DIR}/picopods/usb)
    add_subdirectory(${_pd}/services/miniz      ${CMAKE_BINARY_DIR}/picopods/miniz)

    # Wi-Fi exists only on CYW43 boards (and in the host simulation).
    if((PD_HOST AND NOT EMSCRIPTEN) OR PICO_CYW43_SUPPORTED)
        set(PD_HAS_WIFI ON CACHE INTERNAL "")
    else()
        set(PD_HAS_WIFI OFF CACHE INTERNAL "")
    endif()
    add_subdirectory(${_pd}/services/tls        ${CMAKE_BINARY_DIR}/picopods/tls)
    add_subdirectory(${_pd}/services/git        ${CMAKE_BINARY_DIR}/picopods/git)
    add_subdirectory(${_pd}/services/bt         ${CMAKE_BINARY_DIR}/picopods/bt)
    if(PD_HOST)
        message(STATUS "PicoPods: host build (PC simulator + tests)")
    else()
        message(STATUS "PicoPods: terminal=${PD_TERMINAL} board=${PD_BOARD} wifi=${PD_HAS_WIFI}")
    endif()
endif()

# picopods_add_pod(<name> SOURCES <files...> [LIBS <pd_libs...>]
#                  [TITLE "..."] [VERSION "1.0"] [USB_STDIO])
# Always links pd_app (terminal, ui, gfx, input, fs, core). On the device it
# produces <name>.uf2 (runs from BOOTSEL or the PicoCalc UF2 Loader). On the
# host with SDL2 it also gets the interactive window: the pod's main() is then
# renamed (main=pd_host_app_main) so the window can own the main thread, which
# is why main() must end with a return.
function(picopods_add_pod name)
    cmake_parse_arguments(A "USB_STDIO" "TITLE;VERSION" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${A_SOURCES})
    target_link_libraries(${name} PRIVATE pd_app ${A_LIBS})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>)
    if(NOT A_VERSION)
        set(A_VERSION "0.1")
    endif()
    target_compile_definitions(${name} PRIVATE PD_APP_VERSION="${A_VERSION}")
    if(EMSCRIPTEN)
        # One self-contained file per pod (<name>.pod.js, the wasm inside),
        # loaded by web/run.html: Asyncify lets the pod's blocking main loop
        # yield to the browser (see sys_host.cpp), /sd is IndexedDB-backed.
        target_link_libraries(${name} PRIVATE pd_host_web)
        set_target_properties(${name} PROPERTIES SUFFIX ".pod.js")
        if(NOT A_TITLE)
            set(A_TITLE ${name})
        endif()
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.pod.json
            "{\"name\": \"${name}\", \"title\": \"${A_TITLE}\", \"version\": \"${A_VERSION}\"}\n")
        target_link_options(${name} PRIVATE -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536
            -sMODULARIZE -sEXPORT_NAME=createPod -sSINGLE_FILE -sENVIRONMENT=web
            -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=1048576
            -sEXIT_RUNTIME -lidbfs.js
            "-sEXPORTED_FUNCTIONS=_main,_pd_web_key,_pd_web_frame,_pd_web_frame_generation,_pd_web_width,_pd_web_height"
            "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,HEAPU8,HEAP16,addRunDependency,removeRunDependency")
    elseif(PD_HOST)
        target_compile_options(${name} PRIVATE -g)
        if(PD_HOST_WINDOW)
            target_link_libraries(${name} PRIVATE pd_host_window)
            target_compile_definitions(${name} PRIVATE main=pd_host_app_main)
        endif()
    else()
        target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
        if(NOT A_TITLE)
            set(A_TITLE ${name})
        endif()
        pico_set_program_name(${name} "${A_TITLE}")
        pico_set_program_version(${name} "${A_VERSION}")
        if(A_USB_STDIO)
            pico_enable_stdio_usb(${name} 1)
            pico_enable_stdio_uart(${name} 0)
        else()
            pico_enable_stdio_usb(${name} 0)
            pico_enable_stdio_uart(${name} 1)
        endif()
        pico_add_extra_outputs(${name})
    endif()
endfunction()
