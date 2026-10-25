// TinyUSB configuration for pd_usb_drive: one mass-storage interface.
#pragma once

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENABLED 1
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_MSC 1
#define CFG_TUD_CDC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// Bytes handed to each READ10/WRITE10 callback: 16 sectors, so the SD card
// gets multi-block transfers instead of one sector at a time.
#define CFG_TUD_MSC_EP_BUFSIZE 8192
