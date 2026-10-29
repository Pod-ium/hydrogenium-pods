// pd_usb_drive: the full-screen "USB drive" mode.
#include "pd/usb_drive.h"

#include <cstdio>
#include <string>

#include "msc_backend.h"
#include "pd/app.h"
#include "pd/fs.h"
#include "pd/gfx.h"
#include "pd/sys.h"
#include "pd/terminal.h"

namespace pd::usb {

namespace {

// Keyboard reads are slow (blocking I2C): during transfers read it rarely so
// the USB stack is serviced as often as possible.
constexpr uint32_t kKeysIdleMs = 60;
constexpr uint32_t kKeysBusyMs = 400;
constexpr uint32_t kRedrawMs = 250;
constexpr uint32_t kBusyWindowMs = 300;   // "activity" if a transfer happened this recently

std::string bytes_text(uint64_t n) {
    char b[32];
    if (n < 1024) snprintf(b, sizeof b, "%u B", unsigned(n));
    else if (n < 1024ull * 1024) snprintf(b, sizeof b, "%.1f KB", double(n) / 1024.0);
    else if (n < 1024ull * 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", double(n) / (1024.0 * 1024.0));
    else snprintf(b, sizeof b, "%.2f GB", double(n) / (1024.0 * 1024.0 * 1024.0));
    return b;
}

struct Screen {
    ui::Context& ui;
    const DriveOptions& opt;
    app::StatusBar bar;
    std::string last_state, last_stats, last_diag;
    bool drawn = false;

    Screen(ui::Context& u, const DriveOptions& o) : ui(u), opt(o) {}

    const gfx::Font& big() const { return gfx::font(gfx::FONT_8x16); }
    const gfx::Font& small() const { return gfx::font(gfx::FONT_6x12); }

    int y_state() const { return bar.height() + 158; }

    void draw_static(uint64_t capacity) {
        const ui::Theme& t = ui.theme();
        const int W = gfx::width();
        gfx::clear(t.bg);
        bar.draw(t, opt.title, "", true);

        // A small USB-stick pictogram.
        const int cx = W / 2, top = bar.height() + 16;
        gfx::fill(cx - 22, top, 44, 18, t.dim);             // metal plug
        gfx::fill(cx - 14, top + 5, 8, 6, t.bg);
        gfx::fill(cx + 6, top + 5, 8, 6, t.bg);
        gfx::fill(cx - 32, top + 18, 64, 56, t.accent);     // body
        gfx::fill(cx - 20, top + 34, 40, 4, t.bg);
        gfx::fill(cx - 20, top + 44, 40, 4, t.bg);

        const gfx::Font& f = big();
        int y = top + 88;
        gfx::text_centered(y, f, "USB drive mode", t.fg, t.bg);
        y += f.h + 6;
        gfx::text_centered(y, small(), "Connect the Pico's micro-USB port", t.dim, t.bg);
        y += small().h + 2;
        gfx::text_centered(y, small(), "to the computer.", t.dim, t.bg);

        const int yb = gfx::height() - 3 * small().h - 10;
        gfx::text_centered(yb, small(), "Eject the drive on the computer when done:", t.dim, t.bg);
        gfx::text_centered(yb + small().h + 2, small(), "the PicoCalc returns to the app by itself.", t.dim, t.bg);
        gfx::text_centered(yb + 2 * small().h + 6, small(), "Esc: leave now", t.dim, t.bg);

        const std::string cap = "Card: " + bytes_text(capacity);
        gfx::text_centered(y_state() + 2 * f.h + 4, small(), cap.c_str(), t.dim, t.bg);
        drawn = true;
        last_state.clear();
        last_stats.clear();
        last_diag.clear();
    }

    void draw_dynamic(const detail::Status& s) {
        const ui::Theme& t = ui.theme();
        const int W = gfx::width();
        const gfx::Font& f = big();
        std::string state;
        Color c = t.fg;
        if (!s.media) {
            state = "No SD card";
            c = t.error;
        } else if (s.ejected) {
            state = "Ejected - safe to unplug";
            c = t.accent;
        } else if (!s.configured) {
            state = "Waiting for the computer...";
            c = t.fg;
        } else if (s.suspended) {
            state = "Cable out or computer asleep";
            c = t.dim;
        } else {
            const bool busy = sys::now_ms() - s.last_activity_ms < kBusyWindowMs && s.last_activity_ms != 0;
            state = busy ? "Connected - transferring" : "Connected";
            c = t.accent;
        }
        if (s.error) state += " (card error!)", c = t.error;
        if (state != last_state) {
            gfx::fill(0, y_state(), W, f.h, t.bg);
            gfx::text_centered(y_state(), f, state.c_str(), c, t.bg);
            last_state = state;
        }
        const std::string stats = "Read " + bytes_text(s.read_bytes) + "   Written " + bytes_text(s.written_bytes);
        if (stats != last_stats) {
            const int y = y_state() + f.h + 4;
            gfx::fill(0, y, W, small().h, t.bg);
            gfx::text_centered(y, small(), stats.c_str(), t.fg, t.bg);
            last_stats = stats;
        }
        // Diagnostics: shows how far the computer got if it does not see
        // the drive (no interrupts: nothing on the wire; descriptors asked
        // but no address: replies are not arriving; address but no
        // configuration: stopped half way).
        char d1[64], d2[64];
        snprintf(d1, sizeof d1, "pull-up %s irq %lu task %lu clk %lu", s.pullup ? "on" : "off",
                 (unsigned long)s.irqs, (unsigned long)s.tasks, (unsigned long)s.usb_clk_khz);
        snprintf(d2, sizeof d2, "desc dev %lu cfg %lu str %lu  addr %u", (unsigned long)s.dev_desc,
                 (unsigned long)s.cfg_desc, (unsigned long)s.str_desc, unsigned(s.address));
        const std::string d = std::string(d1) + "|" + d2;
        if (last_diag != d) {
            const int y = y_state() + 2 * f.h + 4 + small().h + 4;
            gfx::fill(0, y, W, 2 * small().h + 2, t.bg);
            gfx::text_centered(y, small(), d1, t.dim, t.bg);
            gfx::text_centered(y + small().h + 2, small(), d2, t.dim, t.bg);
            last_diag = d;
        }
        bar.draw(t, opt.title, "");
    }
};

// Mounts the file system again, showing "Insert an SD card" if needed.
void remount(ui::Context& ui) {
    if (!fs::mount()) app::mount_storage(ui);
}

}  // namespace

bool drive_available() { return detail::available() && terminal().storage() != nullptr; }

DriveExit drive_mode(ui::Context& ui, const DriveOptions& opt) {
    hal::BlockDevice* dev = terminal().storage();
    if (!detail::available() || !dev) return DriveExit::Unavailable;

    // From here on the computer owns the card: nothing on this side may touch
    // the file system until it is mounted again.
    fs::unmount();
    if (!dev->present() || (!dev->ready() && !dev->init())) {
        remount(ui);
        return DriveExit::NoCard;
    }
    if (!detail::start(dev, opt)) {
        remount(ui);
        return DriveExit::Unavailable;
    }

    // Dialogs opened from here (the "leave now?" question) keep USB running.
    auto saved_idle = ui.idle;
    ui.idle = [&saved_idle]() {
        detail::task();
        if (saved_idle) saved_idle();
    };

    Screen scr(ui, opt);
    ui.kbd.flush();
    scr.draw_static(uint64_t(dev->sector_count()) * 512u);
    DriveExit result = DriveExit::Left;
    uint32_t next_keys = 0, next_draw = 0, ejected_at = 0;

    for (;;) {
        sys::sleep_ms(2);
        const detail::Status s = detail::status();
        const uint32_t t = sys::now_ms();

        if (s.ejected) {
            if (!ejected_at) ejected_at = t;
            // Keep answering for a moment (the computer checks the drive
            // after ejecting), then leave.
            if (t - ejected_at > 600) {
                result = DriveExit::Ejected;
                break;
            }
        }
        if (sys::reached(next_draw)) {
            scr.draw_dynamic(s);
            next_draw = t + kRedrawMs;
        }
        if (sys::reached(next_keys)) {
            const bool busy = s.last_activity_ms && t - s.last_activity_ms < kBusyWindowMs;
            next_keys = t + (busy ? kKeysBusyMs : kKeysIdleMs);
            input::KeyEvent ev;
            if (ui.kbd.poll(ev) && ev.code == key::Esc) {
                if (s.configured && !s.ejected && !s.suspended) {
                    const bool leave = ui::confirm(
                        ui, "Leave USB drive mode?",
                        "The computer still has the drive open. Eject it there first, or files it is "
                        "writing may be lost. Leave anyway?");
                    ui.kbd.flush();
                    if (!leave) {
                        scr.draw_static(uint64_t(dev->sector_count()) * 512u);
                        continue;
                    }
                }
                result = DriveExit::Left;
                break;
            }
        }
    }

    detail::stop();
    ui.idle = saved_idle;
    remount(ui);
    ui.kbd.flush();
    return result;
}

}  // namespace pd::usb
