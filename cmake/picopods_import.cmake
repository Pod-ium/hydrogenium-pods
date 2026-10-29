# PicoPods import - copy this file into a pod repository and include() it
# BEFORE project(), like pico_sdk_import.cmake. It finds the PicoPods
# libraries (the hydrogenium-pods repository), sets PICOPODS_PATH and
# includes its picopods_init.cmake:
#
#   cmake_minimum_required(VERSION 3.13)
#   include(picopods_import.cmake)                    # board, terminal, SDK import
#   project(my_pods C CXX ASM)
#   include(${PICOPODS_PATH}/cmake/picopods.cmake)    # SDK init, libraries
#   picopods_add_pod(my_pod SOURCES main.cpp)
#
# Where the libraries come from, first match wins:
#   -DPICOPODS_PATH=/path/to/hydrogenium-pods (or the environment variable)
#   otherwise they are fetched from git (FetchContent) into the build tree:
#     PICOPODS_FETCH_FROM_GIT_URL  (default https://github.com/Pod-ium/hydrogenium-pods)
#     PICOPODS_FETCH_FROM_GIT_TAG  branch, tag or commit (default main)
#   both also read from the environment.
foreach(_v PICOPODS_PATH PICOPODS_FETCH_FROM_GIT_URL PICOPODS_FETCH_FROM_GIT_TAG)
    if(DEFINED ENV{${_v}} AND NOT ${_v})
        set(${_v} $ENV{${_v}})
        message("Using ${_v} from environment ('${${_v}}')")
    endif()
endforeach()
if(NOT PICOPODS_FETCH_FROM_GIT_URL)
    set(PICOPODS_FETCH_FROM_GIT_URL https://github.com/Pod-ium/hydrogenium-pods)
endif()
if(NOT PICOPODS_FETCH_FROM_GIT_TAG)
    set(PICOPODS_FETCH_FROM_GIT_TAG main)
endif()
set(PICOPODS_PATH "${PICOPODS_PATH}" CACHE PATH "Path to the PicoPods libraries (hydrogenium-pods)")
set(PICOPODS_FETCH_FROM_GIT_URL "${PICOPODS_FETCH_FROM_GIT_URL}" CACHE STRING "Git URL of hydrogenium-pods")
set(PICOPODS_FETCH_FROM_GIT_TAG "${PICOPODS_FETCH_FROM_GIT_TAG}" CACHE STRING "hydrogenium-pods branch, tag or commit")

if(NOT PICOPODS_PATH)
    include(FetchContent)
    # A shallow clone can only check out a branch or a tag, not a commit.
    if(PICOPODS_FETCH_FROM_GIT_TAG MATCHES "^[0-9a-f]+$")
        set(_pd_shallow FALSE)
    else()
        set(_pd_shallow TRUE)
    endif()
    message("Downloading PicoPods (${PICOPODS_FETCH_FROM_GIT_URL} @ ${PICOPODS_FETCH_FROM_GIT_TAG})")
    FetchContent_Populate(picopods
        QUIET
        GIT_REPOSITORY ${PICOPODS_FETCH_FROM_GIT_URL}
        GIT_TAG ${PICOPODS_FETCH_FROM_GIT_TAG}
        GIT_SHALLOW ${_pd_shallow}
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/picopods-src
        BINARY_DIR ${CMAKE_BINARY_DIR}/_deps/picopods-build
        SUBBUILD_DIR ${CMAKE_BINARY_DIR}/_deps/picopods-subbuild)
    set(PICOPODS_PATH ${picopods_SOURCE_DIR})
endif()

get_filename_component(PICOPODS_PATH "${PICOPODS_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if(NOT EXISTS ${PICOPODS_PATH}/cmake/picopods_init.cmake)
    message(FATAL_ERROR "'${PICOPODS_PATH}' does not contain the PicoPods libraries (cmake/picopods_init.cmake)")
endif()
set(PICOPODS_PATH ${PICOPODS_PATH} CACHE PATH "Path to the PicoPods libraries (hydrogenium-pods)" FORCE)
include(${PICOPODS_PATH}/cmake/picopods_init.cmake)
