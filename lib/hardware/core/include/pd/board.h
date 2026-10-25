// PicoPods - what the MCU board (the module plugged into the terminal) can do.
//
// A "board" is the programmable module: Pico, Pico W, Pico 2, Pico 2 W, or a
// compatible third-party RP2040/RP2350 board. A "terminal" is the device it
// is plugged into (PicoCalc...). Code that depends on the board checks these
// macros or the functions below, never PICO_BOARD directly.
//
//   PD_CHIP_RP2040 / PD_CHIP_RP2350   exactly one is 1 on the device
//   PD_HAS_WIFI                       CYW43 radio present (Pico W / Pico 2 W)
//   PD_ARCH_ARM / PD_ARCH_RISCV       RP2350 can run either core type
#pragma once
#include <cstdint>

#include "pd/core.h"

#if PD_HOST
#define PD_CHIP_RP2040 0
#define PD_CHIP_RP2350 0
#define PD_HAS_WIFI 0
#define PD_ARCH_ARM 0
#define PD_ARCH_RISCV 0
#else
#include "pico.h"
#if PICO_RP2350
#define PD_CHIP_RP2040 0
#define PD_CHIP_RP2350 1
#else
#define PD_CHIP_RP2040 1
#define PD_CHIP_RP2350 0
#endif
#if defined(PICO_CYW43_SUPPORTED) && PICO_CYW43_SUPPORTED
#define PD_HAS_WIFI 1
#else
#define PD_HAS_WIFI 0
#endif
#if defined(__riscv)
#define PD_ARCH_ARM 0
#define PD_ARCH_RISCV 1
#else
#define PD_ARCH_ARM 1
#define PD_ARCH_RISCV 0
#endif
#endif

namespace pd::board {

const char* name();          // PICO_BOARD name, or "host"
const char* chip();          // "RP2040", "RP2350", "host"
bool has_wifi();
uint32_t flash_size();       // bytes
uint32_t ram_size();         // bytes of on-chip SRAM
uint32_t default_clock_khz();
// A tested overclock for this chip in a PicoCalc (200 MHz on both chips).
// set_clock_khz() handles the voltage.
uint32_t fast_clock_khz();

}  // namespace pd::board
