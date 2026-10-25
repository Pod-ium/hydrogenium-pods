# Terminals. pd_terminal_api is the common header; one pd_terminal_<name>
# library per supported device. cmake/picopods.cmake links the one selected
# with PD_TERMINAL into the pd_terminal target.
add_library(pd_terminal_api INTERFACE)
target_include_directories(pd_terminal_api INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_terminal_api INTERFACE pd_hal pd_input)

if(PD_HOST)
    add_subdirectory(host)
else()
    add_subdirectory(picocalc)
endif()
