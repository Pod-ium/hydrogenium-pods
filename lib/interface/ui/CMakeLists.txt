# pd_ui: themes, menus, dialogs, text input, file browser.
add_library(pd_ui INTERFACE)
target_sources(pd_ui INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/ui.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/themes.cpp)
target_include_directories(pd_ui INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_ui INTERFACE pd_gfx pd_input pd_fs)
