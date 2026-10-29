# pd_hal: abstract hardware interfaces (header only).
add_library(pd_hal INTERFACE)
target_include_directories(pd_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_hal INTERFACE pd_core)
