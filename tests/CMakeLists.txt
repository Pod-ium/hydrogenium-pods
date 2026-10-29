# Host tests of the libraries (cmake -DPD_TERMINAL=host). Run with ctest.
# The scripted sessions of the pods live with the pods, in their own repos.
find_package(Python3 COMPONENTS Interpreter)
set(_tmp ${CMAKE_CURRENT_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${_tmp})

function(pd_host_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LIBS})
    target_compile_options(${name} PRIVATE -g -O1 -fsanitize=address,undefined -Wall)
    target_link_options(${name} PRIVATE -fsanitize=address,undefined)
endfunction()

pd_host_test(core_test SOURCES core_test.cpp LIBS pd_app pd_flash_cache)
add_test(NAME core COMMAND core_test WORKING_DIRECTORY ${_tmp})
set_tests_properties(core PROPERTIES ENVIRONMENT "PD_SDCARD=${_tmp}/sd;PD_KEYS=")

pd_host_test(clock_test SOURCES clock_test.cpp LIBS pd_core)
add_test(NAME clock COMMAND clock_test)

pd_host_test(fs_ops_test SOURCES fs_ops_test.cpp LIBS pd_fs)
add_test(NAME fs_ops COMMAND fs_ops_test ${_tmp}/fs_ops_sd)
file(MAKE_DIRECTORY ${_tmp}/fs_ops_sd)

pd_host_test(text_buffer_test SOURCES text_buffer_test.cpp LIBS pd_text)
add_test(NAME text_buffer COMMAND text_buffer_test)
set_tests_properties(text_buffer PROPERTIES TIMEOUT 240)
pd_host_test(reopen_test SOURCES reopen_test.cpp LIBS pd_text)
add_test(NAME reopen COMMAND reopen_test)
pd_host_test(syntax_test SOURCES syntax_test.cpp LIBS pd_text)
add_test(NAME syntax COMMAND syntax_test)

pd_host_test(epub_test SOURCES epub_test.cpp LIBS pd_epub)
set(_samples big.md code.cpp empty.md long.txt nohead.md odd.txt rich.md)
set(_epubs "")
foreach(s ${_samples})
    get_filename_component(stem ${s} NAME_WE)
    add_test(NAME epub_${stem} COMMAND epub_test ${CMAKE_CURRENT_SOURCE_DIR}/data/${s} ${_tmp}/${stem}.epub)
    list(APPEND _epubs ${_tmp}/${stem}.epub)
endforeach()
if(Python3_FOUND)
    add_test(NAME epub_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_epub.py ${_epubs})
    set_tests_properties(epub_check PROPERTIES DEPENDS "epub_rich;epub_big;epub_long")

    pd_host_test(crosspoint_test SOURCES crosspoint_test.cpp LIBS pd_crosspoint pd_fs)
    add_test(NAME crosspoint COMMAND ${CMAKE_COMMAND}
        -DPY=${Python3_EXECUTABLE} -DMOCK=${CMAKE_CURRENT_SOURCE_DIR}/mock_crosspoint.py
        -DTEST=$<TARGET_FILE:crosspoint_test> -DEPUB=${_tmp}/rich.epub -DROOT=${_tmp}/x3sd
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_crosspoint.cmake)
    set_tests_properties(crosspoint PROPERTIES DEPENDS epub_rich)
endif()

# The demo app, driven by a key script through every screen, must run to the
# end (frames land in tmp/frames as PPM images, handy to eyeball changes).
file(MAKE_DIRECTORY ${_tmp}/demo_sd)
file(WRITE ${_tmp}/demo_sd/notes.md "# My notes\n\nSome *Markdown* with **bold**.\n\n## Chapter two\n\n- one\n- two\n")
set(_keys "enter,a,shift+b,ctrl+c,esc,esc,down,enter,space,down,enter,wait:400,space,wait:400,esc")
set(_keys "${_keys},down,enter,text: Ettore,enter,text:pw,enter,y,enter,down,enter,enter,esc")
set(_keys "${_keys},down,enter,enter,enter,wait:100,enter,down,enter,enter,wait:100,enter")
set(_keys "${_keys},down,enter,esc,down,enter,enter,down,right,shot")
add_test(NAME demo_script COMMAND pd_demo WORKING_DIRECTORY ${_tmp})
set_tests_properties(demo_script PROPERTIES TIMEOUT 60 ENVIRONMENT
    "PD_SDCARD=${_tmp}/demo_sd;PD_FRAMES=${_tmp}/frames;PD_KEYS=${_keys}")

# Git clone against real servers (needs network; enable with -DPD_NET_TESTS=ON).
if(PD_HAS_TLS)
    pd_host_test(git_clone_test SOURCES git_clone_test.cpp LIBS pd_git)
    if(PD_NET_TESTS)
        add_test(NAME git_clone COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_git.sh $<TARGET_FILE:git_clone_test> ${_tmp}/git_sd)
        set_tests_properties(git_clone PROPERTIES TIMEOUT 600)
    endif()
endif()

# Bluetooth OBEX receiver (real OBEX packets from BTstack's builder).
if(PD_HAS_BT)
    pd_host_test(obex_receiver_test SOURCES obex_receiver_test.cpp LIBS pd_bt)
    add_test(NAME obex_receiver COMMAND obex_receiver_test ${_tmp}/obex_sd)
    file(MAKE_DIRECTORY ${_tmp}/obex_sd)
endif()

# BTstack lifecycle: the real BTstack core (HCI, L2CAP, RFCOMM, SDP, GOEP) on a
# dummy radio, started and stopped 3 times under AddressSanitizer. With the
# Pico SDK's teardown it must fail (use-after-free: the HardFault seen on the
# PicoCalc); with pd_bt's it must run clean.
if(PD_HAS_BT AND DEFINED PICO_SDK_PATH)
    set(_B ${PICO_SDK_PATH}/lib/btstack)
    set(_S ${_B}/src)
    add_executable(btstack_lifecycle_test btstack_lifecycle_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../lib/services/bt/src/btstack_modules.cpp
        ${_S}/hci.c ${_S}/hci_cmd.c ${_S}/hci_dump.c ${_S}/l2cap.c ${_S}/l2cap_signaling.c ${_S}/btstack_memory.c
        ${_S}/btstack_memory_pool.c ${_S}/btstack_linked_list.c ${_S}/btstack_util.c ${_S}/btstack_run_loop.c
        ${_S}/btstack_tlv.c ${_S}/ad_parser.c ${_S}/classic/rfcomm.c ${_S}/classic/sdp_server.c ${_S}/classic/sdp_util.c
        ${_S}/classic/sdp_client.c ${_S}/classic/sdp_client_rfcomm.c ${_S}/classic/goep_server.c ${_S}/classic/goep_client.c
        ${_S}/classic/obex_message_builder.c ${_S}/classic/obex_parser.c ${_S}/classic/obex_srm_client.c
        ${_S}/classic/obex_srm_server.c ${_S}/classic/btstack_link_key_db_memory.c ${_S}/classic/gatt_sdp.c
        ${_B}/platform/posix/btstack_run_loop_posix.c)
    target_include_directories(btstack_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/btstack_host ${_S}
        ${_B}/platform/posix ${_B}/3rd-party/bluedroid/encoder/include ${_B}/3rd-party/bluedroid/decoder/include
        ${_B}/3rd-party/micro-ecc ${_B}/3rd-party/rijndael ${_B}/3rd-party/lc3-google/include ${_B}/3rd-party/yxml)
    target_compile_options(btstack_lifecycle_test PRIVATE -fsanitize=address -g -w)
    target_link_options(btstack_lifecycle_test PRIVATE -fsanitize=address)
    add_test(NAME btstack_lifecycle COMMAND btstack_lifecycle_test)
    add_test(NAME btstack_lifecycle_sdk_teardown_crashes COMMAND btstack_lifecycle_test --old)
    set_tests_properties(btstack_lifecycle_sdk_teardown_crashes PROPERTIES WILL_FAIL TRUE)
endif()
