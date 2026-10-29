# pd_net: Wi-Fi + blocking TCP client. Device: CYW43 + lwIP in poll mode on
# Pico W / Pico 2 W; a stub that reports "No Wi-Fi" on other boards. Host:
# POSIX sockets with a simulated radio.
# pd_wifi: saved credentials, network picker, idle power-off (needs pd_ui).
add_library(pd_net INTERFACE)
target_include_directories(pd_net INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_net INTERFACE pd_core)
if(EMSCRIPTEN)
    # A web page cannot open TCP sockets: report "No Wi-Fi", as on a Pico 2.
    target_sources(pd_net INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/net_none.cpp)
elseif(PD_HOST)
    target_sources(pd_net INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/net_posix.cpp)
elseif(PICO_CYW43_SUPPORTED)
    target_sources(pd_net INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/net_cyw43.cpp)
    # config/ holds lwipopts.h (an app may put its own lwipopts.h earlier on its include path)
    target_include_directories(pd_net INTERFACE ${CMAKE_CURRENT_LIST_DIR}/config)
    target_link_libraries(pd_net INTERFACE pico_cyw43_arch_lwip_poll)
else()
    # No radio on this board: every call fails with "No Wi-Fi on this board".
    target_sources(pd_net INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/net_none.cpp)
endif()

add_library(pd_wifi INTERFACE)
target_sources(pd_wifi INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/wifi.cpp)
target_link_libraries(pd_wifi INTERFACE pd_net pd_ui pd_fs)
