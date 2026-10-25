// PicoPods HAL - a source of raw key events.
//
// A driver reports what the keyboard controller reports: key code plus
// pressed / held / released. Modifier tracking, auto-repeat and key state
// live above it, in pd::input::Keyboard, so every terminal behaves the same.
//
// Key codes: PicoPods uses the PicoCalc keyboard codes as its common code set
// (see pd/keys.h). Drivers for other keyboards translate to these codes.
#pragma once
#include <cstdint>

namespace pd::hal {

enum KeyState : uint8_t { KEY_PRESSED = 1, KEY_HELD = 2, KEY_RELEASED = 3 };

struct RawKey {
    uint8_t code = 0;   // pd::key code
    uint8_t state = 0;  // KeyState
};

class KeyboardDevice {
public:
    virtual ~KeyboardDevice() = default;
    // Reads the next queued event. Returns false when there is none. May block
    // for a short time (the PicoCalc controller needs ~16 ms per I2C read).
    virtual bool read(RawKey& ev) = 0;
    // How often read() is worth calling, in ms (throttles synchronous polling).
    virtual uint32_t poll_interval_ms() const { return 4; }
};

}  // namespace pd::hal
