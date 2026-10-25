set(_libs pd_diag pd_flash_cache pd_epub)
if(PD_HAS_WIFI)
    list(APPEND _libs pd_wifi)
endif()
picopods_add_pod(pd_demo
    SOURCES main.cpp
    LIBS ${_libs}
    TITLE "PicoPods demo"
    VERSION "0.1")
if(PD_HAS_WIFI)
    target_compile_definitions(pd_demo PRIVATE PD_DEMO_WIFI=1)
endif()
