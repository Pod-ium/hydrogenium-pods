# pd_text: editor engine from PicoCalc Writer - piece table on the SD card
# (files larger than RAM, undo/redo), soft-wrap layout, streaming syntax
# highlighter (Markdown, XML, Python, C/C++). Pure C++ on pd::fs: host-testable.
add_library(pd_text INTERFACE)
target_sources(pd_text INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/text_buffer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/layout.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/syntax.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/palette.cpp)
target_include_directories(pd_text INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_text INTERFACE pd_fs pd_core)
