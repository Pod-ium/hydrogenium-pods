// USB probe: checks the Pico's native USB with the Pico SDK's stock USB
// serial port (known good on Pico 2 W), showing the USB clock and the USB
// controller state on screen. If the computer sees /dev/ttyACM0 with this,
// the hardware and clocks are fine.
#include <cstdio>
#include <string>

#include "pd/pd.h"

#if !PD_HOST
#include "hardware/clocks.h"
#include "hardware/structs/usb.h"
#include "pico/stdio_usb.h"
#include "tusb.h"
#endif

namespace gfx = pd::gfx;

int main() {
    pd::TerminalConfig cfg;
    cfg.audio = false;
    pd::Terminal& term = pd::terminal_init(cfg);
    gfx::init(term.display());
    pd::ui::Context ui(term.keyboard(), pd::ui::THEME_NIGHT);
    const pd::ui::Theme& t = ui.theme();
    const gfx::Font& big = gfx::font(gfx::FONT_8x16);
    const gfx::Font& small = gfx::font(gfx::FONT_6x12);

    gfx::clear(t.bg);
    int y = 8;
    gfx::text_centered(y, big, "USB probe", t.accent, t.bg);
    y += big.h + 6;
    const char* intro[] = {"Uses the Pico SDK's own USB serial port.",
                           "Connect the Pico's micro-USB port.",
                           "Linux: ls /dev/ttyACM*  then",
                           "       cat /dev/ttyACM0",
                           "Esc: reboot"};
    for (const char* l : intro) {
        gfx::text_centered(y, small, l, t.dim, t.bg);
        y += small.h + 2;
    }
    const int y0 = y + 10;

    uint32_t n = 0, next_print = 0, next_draw = 0;
    for (;;) {
        pd::input::KeyEvent ev;
        if (term.keyboard().poll(ev) && ev.code == pd::key::Esc) pd::sys::reboot();
        const uint32_t now = pd::sys::now_ms();
        bool connected = false;
#if !PD_HOST
        connected = stdio_usb_connected();
#endif
        if (pd::sys::reached(next_print)) {
            next_print = now + 1000;
            printf("PicoPods USB probe: hello %lu\n", (unsigned long)n++);
        }
        if (!pd::sys::reached(next_draw)) {
            pd::sys::sleep_ms(5);
            continue;
        }
        next_draw = now + 300;
        std::vector<std::string> lines;
        char b[64];
#if !PD_HOST
        const uint32_t usb_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
        const uint32_t sys_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
        snprintf(b, sizeof b, "clk_usb %lu kHz (must be ~48000)", (unsigned long)usb_khz);
        lines.push_back(b);
        snprintf(b, sizeof b, "clk_sys %lu kHz", (unsigned long)sys_khz);
        lines.push_back(b);
        snprintf(b, sizeof b, "main_ctrl %08lx  pwr %08lx", (unsigned long)usb_hw->main_ctrl,
                 (unsigned long)usb_hw->pwr);
        lines.push_back(b);
        snprintf(b, sizeof b, "sie_ctrl %08lx", (unsigned long)usb_hw->sie_ctrl);
        lines.push_back(b);
        snprintf(b, sizeof b, "sie_status %08lx", (unsigned long)usb_hw->sie_status);
        lines.push_back(b);
        snprintf(b, sizeof b, "address %lu   enumerated: %s", (unsigned long)(usb_hw->dev_addr_ctrl & 0x7F),
                 tud_mounted() ? "yes" : "no");
        lines.push_back(b);
#endif
        snprintf(b, sizeof b, "messages sent: %lu", (unsigned long)n);
        lines.push_back(b);
        int yy = y0;
        gfx::fill(0, yy - 2, gfx::width(), big.h + 6, t.bg);
        gfx::text_centered(yy, big, connected ? "CONNECTED (serial open)" : "not connected", connected ? t.accent : t.fg,
                           t.bg);
        yy += big.h + 8;
        for (const std::string& l : lines) {
            gfx::fill(0, yy, gfx::width(), small.h, t.bg);
            gfx::text_centered(yy, small, l.c_str(), t.fg, t.bg);
            yy += small.h + 3;
        }
    }
}
