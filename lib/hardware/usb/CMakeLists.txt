# pd_usb_drive: "USB drive" mode. The terminal's SD card becomes a USB mass
# storage device on the board's own USB port (TinyUSB MSC), so a computer can
# read and write it without taking the card out.
#
# Uses the board's native USB, so an app that links it cannot also use
# USB_STDIO in picopods_add_pod() (both would provide the USB descriptors).
# On the host build the mode reports "not available".
add_library(pd_usb_drive INTERFACE)
target_include_directories(pd_usb_drive INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_sources(pd_usb_drive INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/usb_drive.cpp)
target_link_libraries(pd_usb_drive INTERFACE pd_app pd_ui pd_gfx pd_fs pd_terminal pd_core)
if(PD_HOST AND PD_USB_FAKE)
    target_sources(pd_usb_drive INTERFACE ${PD_USB_FAKE})   # layout testing only
elseif(PD_HOST)
    target_sources(pd_usb_drive INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/msc_none.cpp)
else()
    target_sources(pd_usb_drive INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/msc_tinyusb.cpp)
    target_include_directories(pd_usb_drive INTERFACE ${CMAKE_CURRENT_LIST_DIR}/config)
    target_link_libraries(pd_usb_drive INTERFACE tinyusb_device pico_unique_id hardware_clocks)
endif()
