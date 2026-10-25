// PicoPods - text and shapes. The cell renderer comes from PicoCalc Writer
// (one pixel row at a time, colours converted once per cell); the string API
// follows picocalc_ui.
#include "pd/gfx.h"

#include <cstdlib>
#include <cstring>

#include "fonts_terminus.h"

namespace pd::gfx {
namespace {

hal::Display* g_d = nullptr;
uint8_t* g_row = nullptr;       // one panel row in wire format
int g_bpp = 3;
int g_max_cells = 0;

inline int glyph_index(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return int(cp - 0x20);
    if (cp >= 0xA0 && cp <= 0xFF) return int(cp - 0xA0 + 95);
    if (cp == '\t') return 0;
    return '?' - 0x20;
}

inline void put(uint8_t* dst, const uint8_t* px) {
    dst[0] = px[0];
    dst[1] = px[1];
    if (g_bpp == 3) dst[2] = px[2];
}

}  // namespace

const Font& font(int id) {
    if (id < 0 || id >= kFontCount) id = FONT_8x16;
    return *kFonts[id];
}

const char* font_name(int id) {
    static const char* const names[] = {"6x12", "8x16", "10x20", "12x24"};
    return (id >= 0 && id < kFontCount) ? names[id] : "8x16";
}

void init(hal::Display& d) {
    g_d = &d;
    g_bpp = d.bpp();
    free(g_row);
    g_row = static_cast<uint8_t*>(malloc(size_t(d.width()) * size_t(g_bpp)));
    g_max_cells = d.width() / 6 + 1;   // narrowest font is 6 px
}

hal::Display& display() { return *g_d; }
int width() { return g_d ? g_d->width() : 0; }
int height() { return g_d ? g_d->height() : 0; }

// ---------------------------------------------------------------- UTF-8
uint32_t decode_utf8(const uint8_t* s, uint32_t len, int* used) {
    const uint8_t c = s[0];
    int n = 1;
    uint32_t cp = c;
    if (c >= 0xC0 && c < 0xE0) { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c < 0xF0) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c < 0xF8) { n = 4; cp = c & 0x07; }
    else if (c >= 0x80) { *used = 1; return '?'; }
    if (uint32_t(n) > len) { *used = 1; return '?'; }
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *used = 1; return '?'; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *used = n;
    return cp;
}

int text_cells(const char* s) {
    int n = 0;
    for (; *s; ++s)
        if ((uint8_t(*s) & 0xC0) != 0x80) ++n;
    return n;
}

int text_cells(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

size_t byte_of_cell(const std::string& s, int cell) {
    size_t i = 0;
    int c = 0;
    while (i < s.size()) {
        if ((uint8_t(s[i]) & 0xC0) != 0x80) {
            if (c == cell) return i;
            ++c;
        }
        ++i;
    }
    return s.size();
}

std::string clip_cells(const std::string& s, int cells) { return s.substr(0, byte_of_cell(s, cells)); }

std::string pad_cells(const std::string& s, int cells) {
    std::string r = clip_cells(s, cells);
    for (int n = text_cells(r); n < cells; ++n) r += ' ';
    return r;
}

// ---------------------------------------------------------------- text
void draw_cells(int x, int y, int width_px, const Font& f, const Cell* cells, int n, Color bg,
                int cursor_col, Color cursor_color) {
    if (!g_d) return;
    if (x < 0 || y < 0 || y + f.h > g_d->height()) return;
    if (x + width_px > g_d->width()) width_px = g_d->width() - x;
    if (width_px <= 0) return;
    const int h = f.h;
    if (n > width_px / f.w) n = width_px / f.w;
    if (n > g_max_cells) n = g_max_cells;
    if (n < 0) n = 0;

    // Colours are converted once per cell, not once per pixel.
    static uint8_t (*fgc)[3] = nullptr;
    static uint8_t (*bgc)[3] = nullptr;
    static int cap = 0;
    if (cap < g_max_cells) {
        free(fgc);
        free(bgc);
        cap = g_max_cells;
        fgc = static_cast<uint8_t(*)[3]>(malloc(size_t(cap) * 3));
        bgc = static_cast<uint8_t(*)[3]>(malloc(size_t(cap) * 3));
    }
    uint8_t bp[3] = {}, cp[3] = {};
    g_d->encode(bg, bp);
    g_d->encode(cursor_color, cp);
    for (int k = 0; k < n; ++k) {
        g_d->encode(cells[k].fg, fgc[k]);
        g_d->encode(cells[k].bg, bgc[k]);
    }
    int cx = -1;
    if (cursor_col >= 0) {
        cx = cursor_col * f.w;
        if (cx > width_px - 2) cx = width_px - 2;
    }
    const int b = g_bpp;
    g_d->begin(x, y, x + width_px - 1, y + h - 1);
    for (int yy = 0; yy < h; ++yy) {
        for (int i = 0; i < width_px; ++i) put(g_row + b * i, bp);
        for (int k = 0; k < n; ++k) {
            const uint8_t* glyph_row = f.data + glyph_index(cells[k].cp) * h * f.bpr + yy * f.bpr;
            uint8_t* out = g_row + b * k * f.w;
            for (int xx = 0; xx < f.w; ++xx)
                put(out + b * xx, (glyph_row[xx >> 3] & (0x80 >> (xx & 7))) ? fgc[k] : bgc[k]);
        }
        if (cx >= 0)
            for (int xx = cx; xx < cx + 2; ++xx) put(g_row + b * xx, cp);
        g_d->write(g_row, size_t(width_px) * size_t(b));
    }
    g_d->end();
}

int text(int x, int y, int width_px, const Font& f, const std::string& s, Color fg, Color bg,
         int cursor_col, Color cursor_color) {
    if (!g_d) return 0;
    static Cell* cells = nullptr;
    static int cap = 0;
    if (cap < g_max_cells) {
        free(cells);
        cap = g_max_cells;
        cells = static_cast<Cell*>(malloc(sizeof(Cell) * size_t(cap)));
    }
    if (width_px <= 0) width_px = text_cells(s) * f.w;
    int max_cells = width_px / f.w;
    if (max_cells > cap) max_cells = cap;
    int n = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    uint32_t left = uint32_t(s.size());
    while (left > 0 && n < max_cells) {
        int used = 1;
        const uint32_t c = decode_utf8(p, left, &used);
        cells[n++] = Cell{uint16_t(c > 0xFFFF ? '?' : c), fg, bg};
        p += used;
        left -= uint32_t(used);
    }
    draw_cells(x, y, width_px, f, cells, n, bg, cursor_col, cursor_color);
    return n;
}

int text(int x, int y, int width_px, const Font& f, const char* s, Color fg, Color bg, int cursor_col,
         Color cursor_color) {
    return text(x, y, width_px, f, std::string(s ? s : ""), fg, bg, cursor_col, cursor_color);
}

int text_centered(int y, const Font& f, const char* s, Color fg, Color bg) {
    int w = text_cells(s) * f.w;
    if (w > width()) w = width();
    return text((width() - w) / 2, y, w, f, s, fg, bg);
}

// ---------------------------------------------------------------- shapes
void clear(Color c) { fill(0, 0, width(), height(), c); }

void fill(int x, int y, int w, int h, Color c) {
    if (g_d) g_d->fill(x, y, w, h, c);
}

void frame(int x, int y, int w, int h, Color c, int t) {
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y + t, t, h - 2 * t, c);
    fill(x + w - t, y + t, t, h - 2 * t, c);
}

void hline(int x, int y, int w, Color c) { fill(x, y, w, 1, c); }
void vline(int x, int y, int h, Color c) { fill(x, y, 1, h, c); }

void blit_raw(int x, int y, int w, int h, const uint8_t* pixels) {
    if (!g_d || w <= 0 || h <= 0) return;
    g_d->begin(x, y, x + w - 1, y + h - 1);
    g_d->write(pixels, size_t(w) * size_t(h) * size_t(g_bpp));
    g_d->end();
}

void blit_rgb(int x, int y, int w, int h, const Color* pixels) {
    if (!g_d || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > width() || y + h > height()) return;
    g_d->begin(x, y, x + w - 1, y + h - 1);
    for (int r = 0; r < h; ++r) {
        for (int i = 0; i < w; ++i) g_d->encode(pixels[r * w + i], g_row + g_bpp * i);
        g_d->write(g_row, size_t(w) * size_t(g_bpp));
    }
    g_d->end();
}

}  // namespace pd::gfx
