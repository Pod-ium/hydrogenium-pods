// PicoPods - system services: time, sleeping, reboot, clocks, second core.
//
// Implemented by src/sys_rp2.cpp on the device and src/sys_host.cpp on a PC.
#pragma once
#include <cstdint>

namespace pd::sys {

uint32_t now_ms();   // milliseconds since boot (wraps after ~49 days)
uint64_t now_us();   // microseconds since boot
void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);

// True once `deadline` (a now_ms() value) has passed; wrap-safe.
inline bool reached(uint32_t deadline) { return int32_t(now_ms() - deadline) >= 0; }

// Changes the system clock. 0 keeps the default. Raises the core voltage when
// the chip needs it for the requested speed. Returns false if the clock
// cannot be generated exactly (the old clock is then kept).
// Call it before starting peripherals whose timing derives from clk_sys
// (PWM audio, for example). SPI and I2C are re-timed by the drivers.
bool set_clock_khz(uint32_t khz);
uint32_t clock_khz();

// Runs fn on the second core. On the device the second core is also prepared
// for flash writes from the first core (flash_safe_execute). On the host this
// is a detached thread. Only one call is allowed.
bool launch_core1(void (*fn)());

[[noreturn]] void reboot();
// Reboots into the USB mass-storage bootloader (BOOTSEL). Host: exits.
[[noreturn]] void reboot_to_bootloader();

// Watchdog scratch register that survives a soft reset (index 0..7). The SDK
// and the PicoCalc UF2 Loader use some of them: PicoPods only uses index 3.
uint32_t scratch(int index);
void set_scratch(int index, uint32_t value);

// Reset reason bits as reported by the chip (0 on the host).
uint32_t reset_reason();

// Free heap (approximate; mallinfo on the device).
uint32_t free_heap();

}  // namespace pd::sys
