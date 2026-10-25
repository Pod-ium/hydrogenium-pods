#include "pd/app.h"

#include "pd/fs.h"
#include "pd/sys.h"

namespace pd::app {

bool mount_storage(ui::Context& ui) {
    Terminal& t = terminal();
    if (fs::mounted()) return true;
    for (;;) {
        if (fs::mount()) return true;
        if (!t.storage() && !PD_HOST) return false;
        const ui::Theme& th = ui.theme();
        const bool present = !t.storage() || t.storage()->present();
        gfx::clear(th.bg);
        const gfx::Font& f = gfx::font(gfx::FONT_8x16);
        const gfx::Font& s = gfx::font(gfx::FONT_6x12);
        const int y = gfx::height() / 2 - f.h;
        gfx::text_centered(y, f, present ? "SD card not readable" : "Insert an SD card", th.fg, th.bg);
        gfx::text_centered(y + f.h + 4, s, "FAT32 or exFAT", th.dim, th.bg);
        input::KeyEvent ev;
        ui.kbd.wait_for(ev, 1000);
    }
}

void splash(const ui::Theme& t, const char* title, const char* subtitle, bool bars) {
    const gfx::Font& big = gfx::font(gfx::FONT_12x24);
    const gfx::Font& small = gfx::font(gfx::FONT_8x16);
    gfx::clear(t.bg);
    const int y = gfx::height() / 2 - 40;
    gfx::text_centered(y, big, title, t.accent, t.bg);
    if (subtitle) gfx::text_centered(y + big.h + 8, small, subtitle, t.dim, t.bg);
    if (bars) {   // should read red, green, blue, white
        static const Color c[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
        const int w = gfx::width() / 7;
        const int x0 = (gfx::width() - 4 * w - 3 * 8) / 2;
        for (int i = 0; i < 4; ++i) gfx::fill(x0 + i * (w + 8), y + big.h + 40, w, 20, c[i]);
    }
}

std::string battery_text() {
    hal::Power* p = terminal().power();
    if (!p) return "";
    const int b = p->battery_percent();
    if (b < 0) return "";
    return (p->charging() ? "+" : "") + std::to_string(b) + "%";
}

void StatusBar::draw(const ui::Theme& t, const std::string& left, const std::string& right, bool force) {
    const gfx::Font& f = gfx::font(font_id);
    const int cells = gfx::width() / f.w;
    std::string r = right;
    if (battery) {
        const std::string b = battery_text();
        if (!b.empty()) r += (r.empty() ? "" : "  ") + b;
    }
    const int rc = gfx::text_cells(r);
    const std::string line = gfx::pad_cells(left, cells - rc - (rc ? 1 : 0)) + (rc ? " " : "") + r;
    if (!force && line == last_) return;
    last_ = line;
    gfx::fill(0, y, gfx::width(), height(), t.status_bg);
    gfx::text(0, y + 1, gfx::width(), f, line, t.status_fg, t.status_bg);
}

}  // namespace pd::app
