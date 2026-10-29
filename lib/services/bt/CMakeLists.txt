# pd_bt: Bluetooth file transfer (OBEX Object Push, both directions).
# Device (Pico W / Pico 2 W): BTstack Classic on the CYW43; pairing keys in a
# file on the SD card (the SDK's flash storage is not used). Host: a
# simulator backend plus the real OBEX receiver (needs the BTstack sources,
# found in the Pico SDK). Other boards: "no Bluetooth".
add_library(pd_bt INTERFACE)
target_include_directories(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_bt INTERFACE pd_net pd_fs pd_core)

set(_btsrc "")
if(DEFINED PICO_SDK_PATH AND EXISTS ${PICO_SDK_PATH}/lib/btstack/src/btstack.h)
    set(_btsrc ${PICO_SDK_PATH}/lib/btstack)
elseif(DEFINED ENV{PICO_SDK_PATH} AND EXISTS $ENV{PICO_SDK_PATH}/lib/btstack/src/btstack.h)
    set(_btsrc $ENV{PICO_SDK_PATH}/lib/btstack)
endif()

if(PD_HOST)
    if(_btsrc)
        set(PD_HAS_BT ON CACHE INTERNAL "")
        target_include_directories(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/config_host ${_btsrc}/src)
        target_sources(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/bt_sim.cpp
            ${CMAKE_CURRENT_LIST_DIR}/src/obex_receiver.cpp
            ${_btsrc}/src/classic/obex_parser.c ${_btsrc}/src/classic/obex_message_builder.c
            ${_btsrc}/src/btstack_util.c)
    else()
        set(PD_HAS_BT OFF CACHE INTERNAL "")
        message(STATUS "PicoPods: BTstack sources not found (set PICO_SDK_PATH): Bluetooth simulated as absent")
        target_sources(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/bt_none.cpp)
    endif()
elseif(PICO_CYW43_SUPPORTED AND TARGET pico_btstack_classic)
    set(PD_HAS_BT ON CACHE INTERNAL "")
    target_include_directories(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/config)
    target_sources(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/bt_cyw43.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/btstack_glue.cpp ${CMAKE_CURRENT_LIST_DIR}/src/btstack_modules.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/obex_receiver.cpp
        ${_btsrc}/platform/embedded/hal_flash_bank_memory.c ${_btsrc}/platform/embedded/btstack_tlv_flash_bank.c)
    # Not pico_btstack_cyw43: it would bring the flash-based key storage.
    target_link_libraries(pd_bt INTERFACE pico_btstack_classic pico_btstack_base pico_btstack_run_loop_async_context
        pico_btstack_hci_transport_cyw43 pico_btstack_cyw43_headers)
else()
    set(PD_HAS_BT OFF CACHE INTERNAL "")
    target_sources(pd_bt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/bt_none.cpp)
endif()
