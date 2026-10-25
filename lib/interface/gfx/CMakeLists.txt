# pd_gfx: fonts, text, shapes, indexed-framebuffer blitter.
add_library(pd_gfx INTERFACE)
target_sources(pd_gfx INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/gfx.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/blit.cpp)
target_include_directories(pd_gfx INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_gfx INTERFACE pd_hal pd_core)
