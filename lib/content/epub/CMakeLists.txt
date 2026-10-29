# pd_epub: streaming EPUB 2.0.1 writer (Markdown, plain text, source code).
add_library(pd_epub INTERFACE)
target_sources(pd_epub INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/epub.cpp)
target_include_directories(pd_epub INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(pd_epub INTERFACE pd_fs pd_core)
