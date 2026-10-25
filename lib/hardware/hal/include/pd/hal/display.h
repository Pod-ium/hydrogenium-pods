// PicoPods HAL - a pixel display.
//
// Drawing code talks to this interface only. A driver implements the window /
// stream model that SPI panels use: open a rectangle, stream pixels row by row
// (left to right, top to bottom), close it. Pixels are sent in the panel's
// wire format; encode() converts a 0xRRGGBB colour into it.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pd/core.h"

namespace pd::hal {

enum class PixelFormat : uint8_t {
    RGB666,     // 3 bytes R, G, B (top 6 bits used)  - ST7365P / ILI9488 over SPI
    RGB565,     // 2 bytes, big endian                - ST7789, ILI9341...
    RGB888,     // 3 bytes R, G, B                    - host framebuffer
};

constexpr int bytes_per_pixel(PixelFormat f) { return f == PixelFormat::RGB565 ? 2 : 3; }

class Display {
public:
    virtual ~Display() = default;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual PixelFormat format() const = 0;
    int bpp() const { return bytes_per_pixel(format()); }

    // Opens a pixel write into the inclusive window. Must be followed by
    // write()/write_async() calls and end().
    virtual void begin(int x0, int y0, int x1, int y1) = 0;
    virtual void write(const uint8_t* data, size_t bytes) = 0;
    // Starts a background transfer (DMA). The buffer must stay valid until
    // busy() returns false. Drivers without DMA fall back to write().
    virtual void write_async(const uint8_t* data, size_t bytes) { write(data, bytes); }
    virtual bool busy() { return false; }
    // Waits for any transfer in progress, then closes the window.
    virtual void end() = 0;

    // Solid rectangle, clipped to the screen. Drivers may override it with a
    // faster version.
    virtual void fill(int x, int y, int w, int h, Color c);

    // 0..100. Terminals without backlight control ignore it.
    virtual void set_backlight(int percent) { (void)percent; }

    inline void encode(Color c, uint8_t* out) const {
        switch (format()) {
            case PixelFormat::RGB565: {
                const uint16_t v = to565(c);
                out[0] = uint8_t(v >> 8);
                out[1] = uint8_t(v);
                break;
            }
            default:
                out[0] = red(c);
                out[1] = green(c);
                out[2] = blue(c);
                break;
        }
    }
};

// Generic fill using one row of pixels on the stack (up to 480 px wide).
inline void Display::fill(int x, int y, int w, int h, Color c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width()) w = width() - x;
    if (y + h > height()) h = height() - y;
    if (w <= 0 || h <= 0) return;
    uint8_t row[480 * 3];
    const int b = bpp();
    const int chunk = w < 480 ? w : 480;
    uint8_t px[3] = {};
    encode(c, px);
    for (int i = 0; i < chunk; ++i)
        for (int k = 0; k < b; ++k) row[i * b + k] = px[k];
    begin(x, y, x + w - 1, y + h - 1);
    for (int r = 0; r < h; ++r)
        for (int done = 0; done < w; done += chunk) {
            const int n = (w - done) < chunk ? (w - done) : chunk;
            write(row, size_t(n) * size_t(b));
        }
    end();
}

}  // namespace pd::hal
