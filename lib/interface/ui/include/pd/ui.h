// PicoPods - modal UI: menus, dialogs, text input, file browser, progress.
//
// Every call draws a panel over whatever is on screen and blocks on the
// keyboard; the caller repaints its own screen afterwards. Dialogs do not
// discard type-ahead: call ui.kbd.flush() first if stale keys could leak in
// (e.g. when a game opens a pause menu). Layout adapts to
// the display size and the chosen font. Merged from picocalc_ui (menus,
// browser) and PicoCalc Writer (prompt, 3-way confirm, wrapping messages).
//
//   pd::ui::Context ui(terminal.keyboard(), pd::ui::THEME_NIGHT);
//   int volume = 60;
//   pd::ui::MenuItem items[] = {
//       pd::ui::action("Resume", "Esc"),
//       pd::ui::range("Volume", &volume, 0, 100, 10, "%d%%"),
//       pd::ui::action("Quit"),
//   };
//   int picked = pd::ui::menu(ui, "PAUSED", items);    // -1 = Esc
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pd/gfx.h"
#include "pd/input.h"
#include "pd/sys.h"

namespace pd::ui {

// ---------------------------------------------------------------- themes
struct Theme {
    const char* name;
    Color bg, fg, dim;             // main screen
    Color sel, cursor;             // selection background, caret
    Color status_bg, status_fg;    // status bar
    Color panel_bg, panel_fg;      // dialogs
    Color hi_bg, hi_fg;            // highlighted row
    Color accent, error;
};
extern const Theme THEME_NIGHT;      // PicoCalc Writer themes
extern const Theme THEME_PAPER;
extern const Theme THEME_PHOSPHOR;
constexpr int kThemeCount = 3;
const Theme& theme(int index);       // wraps around

// What every dialog needs: where keys come from, colours, font.
struct Context {
    input::Keyboard& kbd;
    const Theme* th;
    int font_id = gfx::FONT_8x16;
    // Called about every millisecond while a dialog waits for a key, so
    // background work keeps running (Wi-Fi polling, audio refill...).
    std::function<void()> idle;
    // Set (from idle()) after drawing over the screen: the dialog waiting in
    // wait_key() receives key::Repaint and redraws itself completely.
    bool repaint = false;

    Context(input::Keyboard& k, const Theme& t) : kbd(k), th(&t) {}
    // Waits for a key, running idle() meanwhile. Dialogs use this.
    input::KeyEvent wait_key() {
        input::KeyEvent ev;
        while (!kbd.poll(ev)) {
            if (idle) idle();
            if (repaint) {
                repaint = false;
                ev = input::KeyEvent();
                ev.code = key::Repaint;
                return ev;
            }
            sys::sleep_ms(1);
        }
        return ev;
    }
    const Theme& theme() const { return *th; }
    const gfx::Font& font() const { return gfx::font(font_id); }
};

// ---------------------------------------------------------------- menu
struct MenuItem {
    enum Kind : uint8_t { ACTION, CHOICE, RANGE } kind = ACTION;
    const char* label = "";
    const char* hint = nullptr;             // ACTION: right-aligned text (a shortcut...)
    int* value = nullptr;                   // CHOICE / RANGE
    const char* const* options = nullptr;   // CHOICE
    int count = 0;                          // CHOICE
    int min = 0, max = 0, step = 1;         // RANGE
    const char* fmt = "%d";                 // RANGE
    bool enabled = true;                    // disabled items are dimmed and skipped
};

MenuItem action(const char* label, const char* hint = nullptr);
MenuItem choice(const char* label, int* value, const char* const* options, int count);
MenuItem range(const char* label, int* value, int min, int max, int step = 1, const char* fmt = "%d");
MenuItem info(const char* label, const char* hint = nullptr);   // a dimmed, unselectable line

struct MenuOptions {
    int initial = 0;
    const char* footer = nullptr;                   // dim line under the items
    std::function<void(int index)> on_change;       // a CHOICE/RANGE value changed
};

// Up/Down move, Enter picks an ACTION (or advances a CHOICE/RANGE), Left/Right
// change a CHOICE/RANGE, Esc/Backspace cancel. Returns the index or -1.
int menu(Context& ui, const char* title, const MenuItem* items, int count,
         const MenuOptions& opt = MenuOptions());
inline int menu(Context& ui, const char* title, const std::vector<MenuItem>& items,
                const MenuOptions& opt = MenuOptions()) {
    return menu(ui, title, items.data(), int(items.size()), opt);
}
template <size_t N>
int menu(Context& ui, const char* title, const MenuItem (&items)[N], const MenuOptions& opt = MenuOptions()) {
    return menu(ui, title, items, int(N), opt);
}

// ---------------------------------------------------------------- dialogs
// Word-wrapped lines (up to 15), then waits for a key.
void message(Context& ui, const char* title, const std::vector<std::string>& lines,
             Color color = kDefaultColor);
// Enter / 'y' -> true; Esc / 'n' -> false.
bool confirm(Context& ui, const char* title, const std::string& question);
// 1 = yes, 0 = no, -1 = cancel (Esc, when allow_cancel).
int ask(Context& ui, const char* title, const std::string& text, bool allow_cancel = true);

// Single-line UTF-8 text input. False if cancelled. mask: shows '*' and Tab
// reveals the text (passwords). Ctrl+U clears.
bool prompt(Context& ui, const char* title, std::string& value, const char* hint = nullptr,
            bool mask = false, size_t max_bytes = 255);

// Progress panel; draw it with done == 0 first, then call with increasing done.
void progress(Context& ui, const char* title, const std::string& label, uint32_t done, uint32_t total);
// A panel for an operation without a known length ("Connecting...").
void busy(Context& ui, const char* title, const std::string& label, bool cancellable = true);
// A progress callback for long operations (TextBuffer, epub, flash cache...):
// draws the panel, redraws at most every 1%, returns false once Esc is pressed
// (if cancellable).
std::function<bool(uint32_t done, uint32_t total)> progress_fn(Context& ui, const char* title,
                                                               const std::string& label,
                                                               bool cancellable = false);

// Scrollable read-only text (help screens, logs). Up/Down/PgUp/PgDn scroll,
// Esc or Enter closes.
void text_view(Context& ui, const char* title, const std::vector<std::string>& lines,
               int font_id = gfx::FONT_6x12);

// ---------------------------------------------------------------- file browser
struct BrowseOptions {
    std::function<bool(const std::string& name)> filter;   // files to list (nullptr = all)
    std::string select;                  // entry to preselect in the starting folder
    const char* empty_hint = nullptr;    // shown when a folder has nothing to list
    bool allow_cancel = true;            // Esc returns false
    bool show_size = true;
    bool show_hidden = false;
    std::vector<std::string> hide;       // names never listed (e.g. an app's work folder)
    size_t max_entries = 500;
};
// Browses from `dir` (updated as the user navigates). True with the chosen
// file's full path in `out`.
bool browse(Context& ui, const char* title, std::string& dir, std::string& out,
            const BrowseOptions& opt = BrowseOptions());
// Filter helper: files whose extension is one of `exts` (".gb,.gbc").
std::function<bool(const std::string&)> extension_filter(const char* exts);

// ---------------------------------------------------------------- helpers
std::vector<std::string> wrap(const std::string& text, int cells);
std::string human_size(uint32_t bytes);   // "512B", "12K", "1.4M"

}  // namespace pd::ui
