# pd_git: Git client (clone over HTTPS, shallow by default) onto the SD card.
# Needs pd_tls (mbedTLS) and pd_miniz.
add_library(pd_git INTERFACE)
target_include_directories(pd_git INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_sources(pd_git INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/pack.cpp ${CMAKE_CURRENT_LIST_DIR}/src/clone.cpp)
target_link_libraries(pd_git INTERFACE pd_tls pd_miniz pd_fs pd_core)
