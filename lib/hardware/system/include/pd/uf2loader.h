// PicoPods - what a firmware must respect to run under the PicoCalc UF2 Loader
// (https://github.com/pelrun/uf2loader), the SD-card bootloader most PicoCalc
// owners have installed.
//
// Standard Pico SDK UF2 files run under it unchanged; only a firmware that
// writes its own flash has to stay out of the loader's way:
//
// - RP2040: the loader sits in the top 16 KB of flash (8 KB loader + 8 KB kept
//   for the SDK's Bluetooth storage on the Pico W). When it installs an app it
//   writes a marker into a hole of the app's vector table: kMagic at
//   XIP_BASE + 0x110, and at XIP_BASE + 0x114 the end of the flash the app may
//   use (an XIP address, e.g. 0x101FC000 on a 2 MB Pico; the loader's README
//   calls it a size, so a plain size is accepted too).
// - RP2350: the app runs from a flash partition that starts after the 8 KB
//   loader, with QMI address translation mapping XIP offset 0 to its start.
//   pd_flash_cache reads that window and writes inside it.
//
// Header only and free of SDK includes, so the RP2040 rule is host-tested.
#pragma once
#include <cstdint>

namespace pd::uf2loader {

constexpr uint32_t kMagic = 0xe98cc638;
constexpr uint32_t kInfoOffset = 0x110;            // RP2040: offset of {magic, end} from XIP_BASE
constexpr uint32_t kRp2040Reserved = 16 * 1024;    // loader + Bluetooth storage at the top
constexpr uint32_t kXipBase = 0x10000000;

// RP2040: bytes from the start of flash that the app may erase and program.
// magic / end are the two words at XIP_BASE + kInfoOffset. Without the marker
// (flashed by BOOTSEL, no loader) the top 16 KB are still left alone, so a
// loader installed later is never damaged.
constexpr uint32_t rp2040_app_flash_end(uint32_t flash_size, uint32_t magic, uint32_t end) {
    uint32_t usable = flash_size > kRp2040Reserved ? flash_size - kRp2040Reserved : 0;
    if (magic == kMagic) {
        const uint32_t e = end >= kXipBase ? end - kXipBase : end;
        if (e > 0 && e < usable) usable = e;
    }
    return usable;
}

}  // namespace pd::uf2loader
