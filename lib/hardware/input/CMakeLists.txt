# pd_input: raw + cooked keyboard layers, background polling on core 1.
add_library(pd_input INTERFACE)
target_sources(pd_input INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/input.cpp)
target_include_directories(pd_input INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_input INTERFACE pd_hal pd_core)
