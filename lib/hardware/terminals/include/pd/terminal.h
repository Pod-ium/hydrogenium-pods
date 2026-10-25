// PicoPods - the terminal: the device the MCU board is plugged into.
//
// A terminal bundles the peripherals an app uses (display, keyboard, storage,
// audio, battery) behind the HAL interfaces. Each supported terminal is one
// library (lib/terminals/<name>) that implements terminal_init(); the build
// picks one with -DPD_TERMINAL=<name>. Apps only include this header:
//
//   int main() {
//       pd::TerminalConfig cfg;
//       cfg.clock_khz = pd::board::fast_clock_khz();
//       pd::Terminal& t = pd::terminal_init(cfg);
//       pd::gfx::init(t.display());
//       pd::ui::Context ui(t.keyboard(), pd::ui::THEME_NIGHT);
//       ...
//   }
#pragma once
#include <cstdint>

#include "pd/board.h"
#include "pd/hal/audio.h"
#include "pd/hal/block_device.h"
#include "pd/hal/display.h"
#include "pd/hal/keyboard.h"
#include "pd/hal/power.h"
#include "pd/input.h"

namespace pd {

struct TerminalConfig {
    uint32_t clock_khz = 0;          // 0 = board default; board::fast_clock_khz() = 200 MHz
    bool keyboard_on_core1 = false;  // poll the keyboard on core 1 (non-blocking main loop)
    bool debug_uart = true;          // stdio on UART0 (115200 baud)
    bool audio = true;               // configure the audio output (started by the app)
    bool storage = true;             // configure the SD card (mounted by the app)
};

// Optional hardware is nullptr when the terminal does not have it.
class Terminal {
public:
    const char* name() const { return name_; }
    hal::Display& display() { return *display_; }
    input::Keyboard& keyboard() { return keyboard_; }
    hal::BlockDevice* storage() { return storage_; }
    hal::AudioOut* audio() { return audio_; }
    hal::Power* power() { return power_; }

    // Battery 0..100 or -1 (convenience over power()).
    int battery_percent() { return power_ ? power_->battery_percent() : -1; }

    // Filled in by the terminal implementation.
    const char* name_ = "?";
    hal::Display* display_ = nullptr;
    hal::KeyboardDevice* keyboard_device_ = nullptr;
    hal::BlockDevice* storage_ = nullptr;
    hal::AudioOut* audio_ = nullptr;
    hal::Power* power_ = nullptr;
    input::Keyboard keyboard_;
};

// Brings the terminal up: clock, debug output, display, keyboard, and the
// pins of the SD card and audio (neither is started). Call once, first.
Terminal& terminal_init(const TerminalConfig& cfg = TerminalConfig());
// The terminal after terminal_init().
Terminal& terminal();

}  // namespace pd
