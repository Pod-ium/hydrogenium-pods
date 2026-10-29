# USB probe: the Pico SDK's own USB serial port (no PicoPods USB code) plus a
# live view of the USB clock and controller state. Tells a firmware problem
# from a hardware / environment one.
picopods_add_pod(pd_usbprobe
    SOURCES src/main.cpp
    TITLE "USB probe"
    VERSION "1.0"
    USB_STDIO)
if(NOT PD_HOST)
    target_link_libraries(pd_usbprobe PRIVATE hardware_clocks)
endif()
