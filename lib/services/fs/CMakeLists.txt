# pd_fs: file system (FatFs on the device, POSIX on the host), paths, settings,
# tree operations (copy / move / delete).
add_library(pd_fs INTERFACE)
target_include_directories(pd_fs INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_sources(pd_fs INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/fs_common.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/fs_ops.cpp)
target_link_libraries(pd_fs INTERFACE pd_hal pd_core)
if(PD_HOST)
    target_sources(pd_fs INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/fs_posix.cpp)
else()
    # FatFs R0.16 (ChaN, BSD-style licence). ffconf.h sits next to ff.c.
    target_sources(pd_fs INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/fs_fatfs.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/diskio.cpp
        ${CMAKE_CURRENT_LIST_DIR}/fatfs/ff.c
        ${CMAKE_CURRENT_LIST_DIR}/fatfs/ffunicode.c)
    target_include_directories(pd_fs INTERFACE ${CMAKE_CURRENT_LIST_DIR}/fatfs)
endif()
