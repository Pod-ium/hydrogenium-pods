// PicoPods - basic types shared by every library.
//
// Nothing here depends on the Pico SDK, so the same headers compile for the
// device and for the host (PD_HOST=1) builds.
#pragma once
#include <cstddef>
#include <cstdint>

#if !defined(PD_HOST)
#define PD_HOST 0
#endif

// Places a hot function in RAM on the device (no-op on the host):
//   void PD_RAM_FUNC(my_function)(int x) { ... }        free functions
//   void PD_RAM_ATTR MyClass::method(int x) { ... }     any function
#if PD_HOST
#define PD_RAM_FUNC(name) name
#define PD_RAM_ATTR
#else
#include "pico.h"
#define PD_RAM_FUNC(name) __not_in_flash_func(name)
#define PD_RAM_ATTR __attribute__((section(".time_critical.pd")))
#endif

namespace pd {

// Colours are always 0xRRGGBB in the API. Each display converts them to its own
// wire format (hal::Display::encode), so application code never sees RGB565,
// RGB666 or byte order.
using Color = uint32_t;

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}
constexpr uint8_t red(Color c) { return uint8_t(c >> 16); }
constexpr uint8_t green(Color c) { return uint8_t(c >> 8); }
constexpr uint8_t blue(Color c) { return uint8_t(c); }

// RGB565 <-> 0xRRGGBB with bit replication (identical to PicoCalc Writer's
// conversion, so a colour looks the same whichever path draws it).
constexpr Color from565(uint16_t c) {
    return (uint32_t((((c >> 11) & 0x1F) << 3) | (((c >> 11) & 0x1F) >> 2)) << 16) |
           (uint32_t((((c >> 5) & 0x3F) << 2) | (((c >> 5) & 0x3F) >> 4)) << 8) |
           uint32_t(((c & 0x1F) << 3) | ((c & 0x1F) >> 2));
}
constexpr uint16_t to565(Color c) {
    return uint16_t(((red(c) & 0xF8) << 8) | ((green(c) & 0xFC) << 3) | (blue(c) >> 3));
}

// Linear blend, t in 0..255 (0 = a, 255 = b).
constexpr Color blend(Color a, Color b, uint8_t t) {
    return rgb(uint8_t((red(a) * (255 - t) + red(b) * t) / 255),
               uint8_t((green(a) * (255 - t) + green(b) * t) / 255),
               uint8_t((blue(a) * (255 - t) + blue(b) * t) / 255));
}

// Sentinel meaning "use the default colour" in APIs that take an optional colour.
constexpr Color kDefaultColor = 0xFFFFFFFFu;

template <typename T, size_t N>
constexpr int array_count(const T (&)[N]) { return int(N); }

}  // namespace pd
