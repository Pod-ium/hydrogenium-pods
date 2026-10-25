# pd_app: umbrella header pd/pd.h + common app helpers (SD mount screen,
# splash, status bar). Linking it pulls in the core libraries and the terminal.
add_library(pd_app INTERFACE)
target_sources(pd_app INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/app.cpp)
target_include_directories(pd_app INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_app INTERFACE pd_terminal pd_ui pd_gfx pd_input pd_fs pd_core)
