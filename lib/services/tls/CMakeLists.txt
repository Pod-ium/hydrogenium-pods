# pd_tls: TLS client (mbedTLS 2.28) over pd::net::Tcp, plus a small HTTP/1.1
# client for http:// and https:// (pd/tls.h). Device: the Pico SDK's mbedTLS
# with config/mbedtls_config.h and built-in root CAs. Host: the system
# mbedTLS 2.28 (Ubuntu: libmbedtls-dev), trusting the system CA store too.
add_library(pd_tls INTERFACE)
target_include_directories(pd_tls INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_sources(pd_tls INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/tls.cpp ${CMAKE_CURRENT_LIST_DIR}/src/http.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ca_bundle.cpp)
target_link_libraries(pd_tls INTERFACE pd_net pd_core)
if(EMSCRIPTEN)
    # Browser build: no system mbedTLS, so compile the Pico SDK's copy (2.28,
    # default config). Nothing connects from a page (pd_net has no Wi-Fi
    # there), but Pod Files links the Git client and needs it to build.
    set(_mbed ${PICO_SDK_PATH}/lib/mbedtls)
    if(NOT EXISTS ${_mbed}/library/ssl_tls.c)
        message(FATAL_ERROR "PicoPods: the browser build needs PICO_SDK_PATH (mbedTLS sources in lib/mbedtls)")
    endif()
    file(GLOB _mbed_src ${_mbed}/library/*.c)
    add_library(pd_mbedtls_web STATIC ${_mbed_src})
    target_include_directories(pd_mbedtls_web PUBLIC ${_mbed}/include)
    target_compile_options(pd_mbedtls_web PRIVATE -w)
    target_link_libraries(pd_tls INTERFACE pd_mbedtls_web)
    set(PD_HAS_TLS ON CACHE INTERNAL "")
elseif(PD_HOST)
    find_library(PD_MBEDTLS_LIB mbedtls)
    find_library(PD_MBEDX509_LIB mbedx509)
    find_library(PD_MBEDCRYPTO_LIB mbedcrypto)
    if(PD_MBEDTLS_LIB AND PD_MBEDX509_LIB AND PD_MBEDCRYPTO_LIB)
        set(PD_HAS_TLS ON CACHE INTERNAL "")
        target_link_libraries(pd_tls INTERFACE ${PD_MBEDTLS_LIB} ${PD_MBEDX509_LIB} ${PD_MBEDCRYPTO_LIB})
    else()
        set(PD_HAS_TLS OFF CACHE INTERNAL "")
        message(STATUS "PicoPods: mbedTLS not found (apt install libmbedtls-dev): TLS/Git apps and tests skipped")
    endif()
else()
    set(PD_HAS_TLS ${PD_HAS_WIFI} CACHE INTERNAL "")
    target_include_directories(pd_tls INTERFACE ${CMAKE_CURRENT_LIST_DIR}/config)
    target_link_libraries(pd_tls INTERFACE pico_mbedtls)
endif()
