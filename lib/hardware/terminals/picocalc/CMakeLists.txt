# pd_terminal_picocalc: ClockworkPi PicoCalc.
add_library(pd_terminal_picocalc INTERFACE)
target_sources(pd_terminal_picocalc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/picocalc.cpp)
target_include_directories(pd_terminal_picocalc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_terminal_picocalc INTERFACE pd_terminal_api pd_drivers pd_fs pd_input pd_core)
