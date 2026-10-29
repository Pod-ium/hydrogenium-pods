// PicoPods - keep one large file from the SD card in flash, memory-mapped.
//
// For data bigger than RAM that must be read at memory speed: game ROMs (what
// PicoBoy uses it for), fonts, dictionaries, lookup tables. The file is copied
// into a region at the top of the flash the app can see (above the firmware)
// and read through XIP. Loading the same, unchanged file again is instant.
//
// Works when the firmware is flashed directly (BOOTSEL / picotool) and when it
// is started by the PicoCalc UF2 Loader (see pd/uf2loader.h): on the RP2350
// the cache stays inside the app's own partition, on the RP2040 below the
// loader at the top of the flash.
//
// Host builds keep the file in RAM instead.
#pragma once
#include <cstdint>
#include <functional>

namespace pd::flash_cache {

// Returns false to cancel. (bytes done, bytes total)
using Progress = std::function<bool(uint32_t done, uint32_t total)>;

// Reserves up to max_bytes (rounded to 64 KB blocks, never overlapping the
// firmware). Call once at start-up.
bool init(uint32_t max_bytes);
uint32_t capacity();

// Copies `path` into flash unless the cache already holds that exact file
// (same path, size, modification stamp and first 512 bytes). Returns a pointer
// to the mapped data, or nullptr with a message in *err.
// If another core is running it must have been started with
// pd::sys::launch_core1 (flash writes pause it safely).
const uint8_t* load(const char* path, uint32_t* size, const Progress& progress = nullptr,
                    const char** err = nullptr);

// Forgets the cached file (the next load() copies again).
void invalidate();

}  // namespace pd::flash_cache
