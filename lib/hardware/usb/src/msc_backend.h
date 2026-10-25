// pd_usb_drive internal: the USB mass-storage backend (TinyUSB on the
// device, a stub on the host).
#pragma once
#include <cstdint>

#include "pd/hal/block_device.h"
#include "pd/usb_drive.h"

namespace pd::usb::detail {

struct Status {
    bool configured = false;   // the computer enumerated the device
    bool suspended = false;    // bus idle: cable out or computer asleep
    bool ejected = false;      // the computer sent "eject"
    bool media = false;        // card present and ready
    bool error = false;        // a read or write to the card failed
    uint64_t read_bytes = 0, written_bytes = 0;
    uint32_t last_activity_ms = 0;
    // Diagnostics: how far the computer got.
    uint32_t irqs = 0;          // USB controller interrupts
    uint32_t dev_desc = 0;      // device descriptor requests
    uint32_t cfg_desc = 0;      // configuration descriptor requests
    uint32_t str_desc = 0;      // string descriptor requests
    uint32_t tasks = 0;         // USB stack runs
    uint8_t address = 0;        // address given by the computer (0 = none yet)
    uint32_t usb_clk_khz = 0;  // measured USB clock (must be ~48000)
    bool pullup = false;        // the device is announcing itself on D+
};

bool available();
bool start(hal::BlockDevice* dev, const DriveOptions& opt);   // attach to the bus
void task();                                                   // nothing to do: USB runs in the background
void stop();                                                   // detach from the bus
Status status();

}  // namespace pd::usb::detail
