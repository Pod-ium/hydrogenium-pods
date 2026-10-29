// PicoPods - "USB drive" mode: the SD card becomes a USB flash drive.
//
// While the mode runs, the board's own USB port (the Pico's micro-USB)
// presents the terminal's storage to a computer as a standard mass-storage
// device: no driver needed on Windows, macOS or Linux. The app's file system
// is unmounted for the whole time, because two file systems writing the same
// card would corrupt it; it is mounted again before drive_mode() returns.
//
//   if (pd::usb::drive_available()) pd::usb::drive_mode(ui);
//
// The mode ends when the computer ejects the drive, or when the user presses
// Esc (asking first if the computer still has the drive open).
#pragma once
#include <cstdint>

#include "pd/ui.h"

namespace pd::usb {

struct DriveOptions {
    const char* vendor = "PicoPods";    // SCSI vendor, up to 8 characters
    const char* product = "SD card";    // SCSI product, up to 16 characters
    const char* manufacturer = "PicoPods";         // USB strings shown by the OS
    const char* usb_product = "PicoCalc SD card";
    const char* title = "USB drive";    // status bar text
    bool read_only = false;             // the computer can only read
};

enum class DriveExit {
    Unavailable,   // no native USB in this build, or no storage
    NoCard,        // no readable card
    Ejected,       // the computer ejected the drive (clean)
    Left,          // the user pressed Esc
};

// True when this build can run the mode (device build with pd_usb_drive).
bool drive_available();

// Runs the full-screen mode until it ends. The caller redraws its screen and
// re-reads any folder listing afterwards: the computer may have changed
// anything on the card.
DriveExit drive_mode(ui::Context& ui, const DriveOptions& opt = DriveOptions());

}  // namespace pd::usb
