// PicoPods - indexed framebuffer blitter (PicoBoy's frame streaming, generalised).
#include "pd/blit.h"

#include <cstdlib>
#include <cstring>

namespace pd {

size_t IndexedBlitter::memory_needed(const Config& c, int bpp) {
    return size_t(c.w) * c.scale * bpp * c.scale * size_t(c.h) + size_t(c.w) * size_t(c.h);
}

bool IndexedBlitter::begin(hal::Display& d, const Config& c) {
    release();
    if (c.w <= 0 || c.h <= 0 || c.scale < 1 || c.scale > 4) return false;
    if (c.x < 0 || c.y < 0 || c.x + c.w * c.scale > d.width() || c.y + c.h * c.scale > d.height()) return false;
    d_ = &d;
    c_ = c;
    bpp_ = d.bpp();
    line_bytes_ = size_t(c.w) * c.scale * bpp_ * c.scale;
    out_ = static_cast<uint8_t*>(malloc(line_bytes_ * size_t(c.h)));
    sent_ = static_cast<uint8_t*>(malloc(size_t(c.w) * size_t(c.h)));
    if (!out_ || !sent_) {
        release();
        return false;
    }
    sent_valid_ = false;
    busy_ = false;
    return true;
}

void IndexedBlitter::release() {
    if (busy_) wait();
    free(out_);
    free(sent_);
    out_ = sent_ = nullptr;
    d_ = nullptr;
}

void IndexedBlitter::set_palette(const Color* rgb, int n) {
    wait();
    if (n > 256) n = 256;
    for (int i = 0; i < n; ++i) d_->encode(rgb[i], pal_[i]);
    sent_valid_ = false;
}

void IndexedBlitter::invalidate() { sent_valid_ = false; }

void PD_RAM_ATTR IndexedBlitter::convert_line(const uint8_t* fb, int y) {
    const uint8_t* src = fb + y * c_.w;
    uint8_t* row = out_ + size_t(y) * line_bytes_;
    uint8_t* o = row;
    const int s = c_.scale;
    if (bpp_ == 3 && s == 2) {   // the common PicoCalc case, unrolled
        for (int x = 0; x < c_.w; ++x) {
            const uint8_t* p = pal_[src[x]];
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[0]; o[4] = p[1]; o[5] = p[2];
            o += 6;
        }
    } else {
        for (int x = 0; x < c_.w; ++x) {
            const uint8_t* p = pal_[src[x]];
            for (int k = 0; k < s; ++k)
                for (int b = 0; b < bpp_; ++b) *o++ = p[b];
        }
    }
    const size_t panel_row = size_t(c_.w) * s * bpp_;
    for (int k = 1; k < s; ++k) memcpy(row + k * panel_row, row, panel_row);   // vertical scale
}

void IndexedBlitter::poll() {
    if (busy_ && !d_->busy()) {
        d_->end();
        busy_ = false;
    }
}

bool IndexedBlitter::busy() {
    poll();
    return busy_;
}

void IndexedBlitter::wait() {
    while (busy_) poll();
}

bool IndexedBlitter::submit(const uint8_t* fb) {
    if (!d_) return false;
    poll();
    if (busy_) return false;
    int first = 0, last = c_.h - 1;
    if (sent_valid_) {
        while (first < c_.h && memcmp(fb + first * c_.w, sent_ + first * c_.w, size_t(c_.w)) == 0) ++first;
        if (first == c_.h) return true;   // nothing changed
        while (last > first && memcmp(fb + last * c_.w, sent_ + last * c_.w, size_t(c_.w)) == 0) --last;
    }
    for (int y = first; y <= last; ++y) convert_line(fb, y);
    memcpy(sent_ + first * c_.w, fb + first * c_.w, size_t(last - first + 1) * size_t(c_.w));
    sent_valid_ = true;

    const int s = c_.scale;
    d_->begin(c_.x, c_.y + first * s, c_.x + c_.w * s - 1, c_.y + last * s + s - 1);
    busy_ = true;
    d_->write_async(out_ + size_t(first) * line_bytes_, size_t(last - first + 1) * line_bytes_);
    return true;
}

}  // namespace pd
