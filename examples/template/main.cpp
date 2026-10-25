// PicoPods pod template - copy this folder to start a new pod.
//
// What it shows: bringing the terminal up, the start-up log, mounting the SD
// card, settings that survive a reboot, a main loop that redraws only when
// needed, a status bar, and an Esc menu.
#include <string>

#include "pd/diag.h"
#include "pd/pd.h"

namespace {

const char* const kStages[] = {"start", "display", "SD card", "settings", "ready"};
enum Stage { kStart, kDisplay, kSd, kSettings, kReady };

const char* const kWorkDir = "/.template";            // app data on the SD card
const char* const kSettingsPath = "/.template/settings.txt";

struct State {
    int theme = 0;
    int counter = 0;
    std::string last_file;
};

void load(State& s) {
    pd::Settings st;
    st.load(kSettingsPath);
    s.theme = st.get_int("theme", 0, 0, pd::ui::kThemeCount - 1);
    s.counter = st.get_int("counter", 0);
    s.last_file = st.get("last_file");
}

void save(const State& s) {
    pd::Settings st;
    st.set_int("theme", s.theme);
    st.set_int("counter", s.counter);
    st.set("last_file", s.last_file);
    st.save(kSettingsPath);
}

void draw(pd::ui::Context& ui, const State& s, pd::app::StatusBar& bar) {
    const pd::ui::Theme& t = ui.theme();
    const pd::gfx::Font& f = pd::gfx::font(pd::gfx::FONT_8x16);
    pd::gfx::clear(t.bg);
    bar.draw(t, "Template", "Esc menu", true);
    int y = bar.height() + 20;
    pd::gfx::text(16, y, 0, f, "Hello from PicoPods!", t.accent, t.bg);
    y += 2 * f.h;
    pd::gfx::text(16, y, 0, f, "Space: count   Enter: open a file", t.fg, t.bg);
    y += 2 * f.h;
    pd::gfx::text(16, y, 0, f, "Counter: " + std::to_string(s.counter), t.fg, t.bg);
    y += f.h;
    pd::gfx::text(16, y, pd::gfx::width() - 32, f, "Last file: " + (s.last_file.empty() ? std::string("-") : s.last_file),
                  t.dim, t.bg);
}

}  // namespace

int main() {
    pd::diag::begin("Template", PD_APP_VERSION, kStages, pd::array_count(kStages));
    pd::diag::stage(kStart);

    pd::TerminalConfig cfg;
    pd::Terminal& term = pd::terminal_init(cfg);
    pd::gfx::init(term.display());
    pd::diag::display_ready();
    pd::diag::stage(kDisplay);

    State s;
    pd::ui::Context ui(term.keyboard(), pd::ui::THEME_NIGHT);

    pd::diag::stage(kSd);
    pd::app::mount_storage(ui);
    pd::fs::mkdirs(kWorkDir);

    pd::diag::stage(kSettings);
    load(s);
    ui.th = &pd::ui::theme(s.theme);

    pd::diag::stage(kReady);
    pd::diag::done();

    pd::app::StatusBar bar;
    std::string dir = "/";
    bool dirty = true;
    for (;;) {
        pd::input::KeyEvent ev;
        if (term.keyboard().poll(ev)) {
            if (ev.code == pd::key::Space) {
                ++s.counter;
                dirty = true;
            } else if (ev.code == pd::key::Enter) {
                std::string file;
                if (pd::ui::browse(ui, "Open", dir, file)) s.last_file = file;
                dirty = true;
            } else if (ev.code == pd::key::Esc) {
                const char* const themes[] = {"Night", "Paper", "Phosphor"};
                pd::ui::MenuItem items[] = {
                    pd::ui::action("Resume", "Esc"),
                    pd::ui::choice("Theme", &s.theme, themes, 3),
                    pd::ui::action("About"),
                    pd::ui::action("Save and reboot"),
                };
                pd::ui::MenuOptions opt;
                opt.on_change = [&](int) { ui.th = &pd::ui::theme(s.theme); };
                const int r = pd::ui::menu(ui, "MENU", items, opt);
                if (r == 2)
                    pd::ui::message(ui, "About", {std::string("Template ") + PD_APP_VERSION,
                                                  std::string("Board: ") + pd::board::name() + " (" + pd::board::chip() + ")",
                                                  std::string("Terminal: ") + term.name()});
                if (r == 3) {
                    save(s);
                    pd::sys::reboot();
                }
                save(s);
                dirty = true;
            }
            continue;
        }
        if (dirty) {
            draw(ui, s, bar);
            dirty = false;
        } else {
            bar.draw(ui.theme(), "Template", "Esc menu");   // battery changes
        }
        pd::sys::sleep_ms(5);
    }
}
