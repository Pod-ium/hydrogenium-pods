// PicoPods - text and shape drawing on any hal::Display.
//
// There is no framebuffer: every call streams pixels straight to the panel
// through one row buffer (a few hundred bytes). Text comes from four built-in
// Terminus bitmap fonts covering ASCII and Latin-1; strings are UTF-8, other
// code points are drawn as '?'.
//
//   pd::gfx::init(terminal.display());
//   pd::gfx::clear(0x000000);
//   pd::gfx::text(10, 10, 0, pd::gfx::font(pd::gfx::FONT_8x16), "Hello", 0xFFFFFF, 0x000000);
//
// The per-cell API (draw_cells) gives each character its own colours and an
// optional caret, which is how an editor draws a syntax-coloured line in one pass.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "pd/core.h"
#include "pd/hal/display.h"

namespace pd::gfx {

struct Font {
    uint8_t w, h, bpr;       // cell width/height in pixels, bytes per glyph row
    const uint8_t* data;     // 190 glyphs: U+0020..U+007E, then U+00A0..U+00FF
};

enum FontId { FONT_6x12 = 0, FONT_8x16 = 1, FONT_10x20 = 2, FONT_12x24 = 3, FONT_COUNT = 4 };
const Font& font(int id);    // out-of-range ids give 8x16
const char* font_name(int id);   // "6x12"...

struct Cell {
    uint16_t cp;     // code point (Latin-1 range drawn, others as '?')
    Color fg, bg;
};

// ---- setup
void init(hal::Display& d);
hal::Display& display();
int width();
int height();

// ---- UTF-8 helpers
// Decodes one sequence from s (len bytes available); *used gets its length (>= 1).
uint32_t decode_utf8(const uint8_t* s, uint32_t len, int* used);
int text_cells(const char* s);                    // characters in a UTF-8 string
int text_cells(const std::string& s);
size_t byte_of_cell(const std::string& s, int cell);  // byte offset of the n-th character
std::string clip_cells(const std::string& s, int cells);
std::string pad_cells(const std::string& s, int cells);   // clip, then pad with spaces

// ---- text
// Draws n cells at (x, y) inside a band width_px wide that is first filled
// with bg. Only whole cells are drawn. cursor_col >= 0 draws a 2 px caret at
// the left edge of that cell.
void draw_cells(int x, int y, int width_px, const Font& f, const Cell* cells, int n, Color bg,
                int cursor_col = -1, Color cursor_color = 0);

// Draws a UTF-8 string clipped to width_px (width_px <= 0: exactly as wide as
// the text). Returns the number of characters drawn.
int text(int x, int y, int width_px, const Font& f, const char* s, Color fg, Color bg,
         int cursor_col = -1, Color cursor_color = 0);
int text(int x, int y, int width_px, const Font& f, const std::string& s, Color fg, Color bg,
         int cursor_col = -1, Color cursor_color = 0);
// Horizontally centred on the screen.
int text_centered(int y, const Font& f, const char* s, Color fg, Color bg);

// ---- shapes
void clear(Color c);
void fill(int x, int y, int w, int h, Color c);
void frame(int x, int y, int w, int h, Color c, int thickness = 1);
void hline(int x, int y, int w, Color c);
void vline(int x, int y, int h, Color c);
// Pixels already in the display's wire format (see hal::Display::encode).
void blit_raw(int x, int y, int w, int h, const uint8_t* pixels);
// 0xRRGGBB pixels (converted row by row).
void blit_rgb(int x, int y, int w, int h, const Color* pixels);

}  // namespace pd::gfx
