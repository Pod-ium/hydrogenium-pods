// PicoPods - small helpers most apps need, on top of the other libraries.
#pragma once
#include <string>

#include "pd/terminal.h"
#include "pd/ui.h"

namespace pd::app {

// Mounts the SD card. While it fails, shows "Insert an SD card" (or "not
// readable") and retries on a key press or every second. Returns false only
// if the terminal has no storage at all.
bool mount_storage(ui::Context& ui);

// Full-screen splash: title, subtitle, optional colour bars (useful to check
// the panel's colour order on new hardware). Does not wait.
void splash(const ui::Theme& t, const char* title, const char* subtitle, bool colour_bars = false);

// One-line status bar at the top or bottom of the screen: left text, right
// text, battery on the far right if the terminal reports it.
struct StatusBar {
    int y = 0;                 // top of the bar
    int font_id = gfx::FONT_6x12;
    bool battery = true;
    int height() const { return gfx::font(font_id).h + 2; }
    // Redraws only if the text changed (or force).
    void draw(const ui::Theme& t, const std::string& left, const std::string& right, bool force = false);

private:
    std::string last_;
};

// "73%" or "+73%" while charging, "" if unknown.
std::string battery_text();

}  // namespace pd::app
