# pd_crosspoint: HTTP upload client for e-readers running CrossPoint (Xteink X3/X4).
add_library(pd_crosspoint INTERFACE)
target_sources(pd_crosspoint INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/crosspoint.cpp)
target_include_directories(pd_crosspoint INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_crosspoint INTERFACE pd_net)
