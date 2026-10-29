// PicoPods - fast scaled output of an 8-bit indexed framebuffer.
//
// Made for emulators and games (it is PicoBoy's video path, generalised): the
// app renders into a w x h buffer of palette indices, submit() converts only
// the band of lines that changed since the last frame, scales it by 1..4 and
// sends it with one background (DMA) transfer. The CPU is free while the
// panel receives the frame; poll()/busy() tell when the next one can go.
//
//   pd::IndexedBlitter blit;
//   blit.begin(display, {160, 144, 2, 0, 16});   // 160x144, x2, at (0, 16)
//   blit.set_palette(colors, 4);
//   for (;;) { render(fb); if (!blit.busy()) blit.submit(fb); }
//
// Memory: (w * scale * bpp) * (h * scale) bytes for the converted frame plus
// w * h for change detection. 160x144 x2 on the PicoCalc = 276 KB + 23 KB.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pd/core.h"
#include "pd/hal/display.h"

namespace pd {

class IndexedBlitter {
public:
    struct Config {
        int w = 0, h = 0;      // source size
        int scale = 1;         // 1..4
        int x = 0, y = 0;      // top-left on screen
    };

    ~IndexedBlitter() { release(); }
    static size_t memory_needed(const Config& c, int bpp);
    // Allocates the buffers. False if they do not fit (RAM or screen).
    bool begin(hal::Display& d, const Config& c);
    void release();

    void set_palette(const Color* rgb, int n);   // n <= 256; waits for the current frame
    // Starts sending fb (w*h indices). False if a frame is still in flight.
    // Returns true (and sends nothing) when nothing changed.
    bool submit(const uint8_t* fb);
    void poll();          // finishes a completed transfer; cheap, call often
    bool busy();
    void wait();          // blocks until the current frame is on screen
    void invalidate();    // the next submit() redraws everything

private:
    void convert_line(const uint8_t* fb, int y);

    hal::Display* d_ = nullptr;
    Config c_;
    int bpp_ = 3;
    size_t line_bytes_ = 0;    // one scaled source line: w*scale*bpp*scale
    uint8_t* out_ = nullptr;
    uint8_t* sent_ = nullptr;
    uint8_t pal_[256][3] = {};
    bool sent_valid_ = false;
    bool busy_ = false;
};

}  // namespace pd
