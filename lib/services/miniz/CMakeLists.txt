# pd_miniz: zlib-compatible inflate/deflate (miniz 3.0.2, MIT, third party).
# Only the raw tinfl/tdefl engines are used; archive/stdio/time APIs are off.
add_library(pd_miniz INTERFACE)
target_include_directories(pd_miniz INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
target_sources(pd_miniz INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/miniz_build.c)
target_compile_definitions(pd_miniz INTERFACE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
