// PicoPods demo - a tour of the libraries, and a quick hardware check for a new
// board or terminal: keyboard, display, audio, SD card, flash, Wi-Fi.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "pd/blit.h"
#include "pd/diag.h"
#include "pd/epub.h"
#include "pd/flash_cache.h"
#include "pd/pd.h"
#if PD_DEMO_WIFI
#include "pd/wifi.h"
#endif

using namespace pd;

namespace {

const char* const kStages[] = {"start", "display", "SD card", "menu"};
Terminal* g_term = nullptr;

// ---------------------------------------------------------------- keyboard
void keyboard_test(ui::Context& ui) {
    const ui::Theme& t = ui.theme();
    const gfx::Font& f = gfx::font(gfx::FONT_8x16);
    input::Keyboard& kb = ui.kbd;
    std::vector<std::string> log;
    gfx::clear(t.bg);
    gfx::text(8, 4, 0, f, "Keyboard test - raw events", t.accent, t.bg);
    gfx::text(8, gfx::height() - f.h - 4, 0, f, "Esc twice to leave", t.dim, t.bg);
    kb.flush();
    int esc = 0;
    for (;;) {
        hal::RawKey ev;
        if (!kb.poll_raw(ev)) {
            sys::sleep_ms(2);
            continue;
        }
        const char* st = ev.state == hal::KEY_PRESSED ? "down" : ev.state == hal::KEY_RELEASED ? "up" : "hold";
        const input::Modifiers m = kb.mods();
        char line[64];
        snprintf(line, sizeof line, "%-6s %-5s 0x%02X  %s%s%s", key::name(ev.code), st, ev.code, m.ctrl ? "Ctrl " : "",
                 m.shift ? "Shift " : "", m.alt ? "Alt" : "");
        log.push_back(line);
        if (log.size() > 15) log.erase(log.begin());
        for (size_t i = 0; i < 15; ++i)
            gfx::text(8, 28 + int(i) * f.h, gfx::width() - 16, f, i < log.size() ? log[i] : "", t.fg, t.bg);
        if (ev.code == key::Esc && ev.state == hal::KEY_PRESSED) {
            if (++esc == 2) return;
        } else if (ev.state == hal::KEY_PRESSED) {
            esc = 0;
        }
    }
}

// ---------------------------------------------------------------- display
void fonts_and_colours(ui::Context& ui) {
    const ui::Theme& t = ui.theme();
    gfx::clear(t.bg);
    int y = 4;
    for (int i = 0; i < gfx::FONT_COUNT; ++i) {
        const gfx::Font& f = gfx::font(i);
        gfx::text(8, y, gfx::width() - 16, f, std::string(gfx::font_name(i)) + " Àèìòù ÿ © 0123 {}", t.fg, t.bg);
        y += f.h + 4;
    }
    // Per-cell colours and a caret, as an editor draws a line.
    const gfx::Font& f = gfx::font(gfx::FONT_8x16);
    const char* word = "per-cell colours";
    gfx::Cell cells[16];
    for (int i = 0; i < 16; ++i) cells[i] = {uint16_t(word[i]), rgb(uint8_t(255 - i * 12), uint8_t(i * 16), 200), t.bg};
    gfx::draw_cells(8, y, 16 * f.w, f, cells, 16, t.bg, 3, t.cursor);
    y += f.h + 8;
    // Gradient bars
    const int w = gfx::width() - 16;
    std::vector<Color> row(static_cast<size_t>(w));
    const Color ends[3][2] = {{0x000000, 0xFF0000}, {0x000000, 0x00FF00}, {0x000000, 0x0000FF}};
    for (int b = 0; b < 3; ++b) {
        for (int x = 0; x < w; ++x) row[size_t(x)] = blend(ends[b][0], ends[b][1], uint8_t(x * 255 / (w - 1)));
        for (int r = 0; r < 12; ++r) gfx::blit_rgb(8, y + r, w, 1, row.data());
        y += 14;
    }
    for (int i = 0; i < ui::kThemeCount; ++i) {
        const ui::Theme& th = ui::theme(i);
        const int x = 8 + i * ((gfx::width() - 16) / 3);
        gfx::fill(x, y, 96, 36, th.bg);
        gfx::text(x + 4, y + 2, 88, gfx::font(0), th.name, th.fg, th.bg);
        gfx::text(x + 4, y + 18, 88, gfx::font(0), "accent", th.accent, th.bg);
    }
    ui.kbd.flush();
    ui.kbd.wait();
}

// ---------------------------------------------------------------- blitter + audio
void plasma(ui::Context& ui) {
    const ui::Theme& t = ui.theme();
    constexpr int W = 160, H = 120, S = 2;
    static uint8_t fb[W * H];
    IndexedBlitter blit;
    const int y0 = (gfx::height() - H * S) / 2;
    if (!blit.begin(gfx::display(), {W, H, S, (gfx::width() - W * S) / 2, y0})) {
        ui::message(ui, "Plasma", {"Not enough memory for the frame buffers."});
        return;
    }
    Color pal[256];
    for (int i = 0; i < 256; ++i) {
        const float a = float(i) * 6.2831853f / 256.f;
        pal[i] = rgb(uint8_t(128 + 127 * std::sin(a)), uint8_t(128 + 127 * std::sin(a + 2.1f)),
                     uint8_t(128 + 127 * std::sin(a + 4.2f)));
    }
    blit.set_palette(pal, 256);
    gfx::clear(0x000000);
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    gfx::text(4, 2, 0, f, "Indexed blitter 160x120 x2 - Space: tone  Esc: back", t.dim, 0x000000);

    hal::AudioOut* audio = g_term->audio();
    bool tone = false;
    const uint32_t rate = 22050;
    if (audio) audio->start(rate);
    std::vector<int16_t> samples;
    uint32_t phase = 0;

    ui.kbd.flush();
    uint32_t frames = 0, shown = 0, t0 = sys::now_ms();
    for (int n = 0;; ++n) {
        input::KeyEvent ev;
        if (ui.kbd.poll(ev)) {
            if (ev.code == key::Esc) break;
            if (ev.code == key::Space) tone = !tone;
        }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int v = int(64 * std::sin((x + n) * 0.06f) + 64 * std::sin((y - n) * 0.05f) +
                                  64 * std::sin((x + y + 2 * n) * 0.04f));
                fb[y * W + x] = uint8_t(v + n * 2);
            }
        ++frames;
        if (!blit.busy()) {
            blit.submit(fb);
            ++shown;
        }
        if (audio) {   // a 440 Hz square wave, paced by the audio clock
            const int target = std::min(1100, audio->capacity() - 256);
            while (audio->buffered() < target) {
                samples.assign(256 * 2, 0);
                for (int i = 0; i < 256; ++i) {
                    const int16_t v = tone ? ((phase++ * 440 * 2 / rate) & 1 ? 6000 : -6000) : 0;
                    samples[size_t(i) * 2] = samples[size_t(i) * 2 + 1] = v;
                }
                audio->push(samples.data(), 256);
            }
        }
        if (sys::now_ms() - t0 >= 1000) {
            char s[40];
            snprintf(s, sizeof s, "%u fps rendered, %u shown  ", unsigned(frames), unsigned(shown));
            gfx::text(4, gfx::height() - f.h - 2, 0, f, s, t.dim, 0x000000);
            frames = shown = 0;
            t0 = sys::now_ms();
        }
    }
    blit.wait();
    if (audio) audio->stop();
}

// ---------------------------------------------------------------- files
void view_file(ui::Context& ui, std::string& dir) {
    std::string file;
    ui::BrowseOptions o;
    o.filter = ui::extension_filter(".txt,.md,.cfg,.ini,.log,.py,.cpp,.h");
    o.empty_hint = "No text files here";
    if (!ui::browse(ui, "View", dir, file, o)) return;
    std::string text;
    fs::File f;
    if (!f.open(file, fs::Mode::Read)) {
        ui::message(ui, "View", {"Cannot open " + file, fs::last_error_text()}, ui.theme().error);
        return;
    }
    text.resize(std::min<uint32_t>(f.size(), 16384));
    text.resize(size_t(std::max(0, f.read(&text[0], uint32_t(text.size())))));
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string l = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    ui::text_view(ui, path::base_name(file).c_str(), lines);
}

void export_epub(ui::Context& ui, std::string& dir) {
    std::string file;
    ui::BrowseOptions o;
    o.filter = ui::extension_filter(".txt,.md");
    if (!ui::browse(ui, "Export to ePub", dir, file, o)) return;
    fs::File f;
    if (!f.open(file, fs::Mode::Read)) return;
    const epub::Reader rd = [&](uint32_t pos, uint8_t* buf, uint32_t n) -> uint32_t {
        if (!f.seek(pos)) return 0;
        const int r = f.read(buf, n);
        return r > 0 ? uint32_t(r) : 0;
    };
    epub::Options eo;
    eo.kind = epub::kindForPath(file);
    eo.title = epub::guessTitle(rd, f.size(), eo.kind, path::stem(file));
    if (!ui::prompt(ui, "ePub title", eo.title)) return;
    const std::string out = path::with_extension(file, ".epub");
    std::string err;
    if (epub::write(rd, f.size(), eo, out, &err, ui::progress_fn(ui, "Exporting", path::base_name(out), true)))
        ui::message(ui, "ePub", {"Written " + out});
    else
        ui::message(ui, "ePub", {"Failed: " + err}, ui.theme().error);
}

void flash_test(ui::Context& ui, std::string& dir) {
    std::string file;
    if (!ui::browse(ui, "Load into flash", dir, file)) return;
    const uint32_t t0 = sys::now_ms();
    uint32_t size = 0;
    const char* err = nullptr;
    const uint8_t* p = flash_cache::load(file.c_str(), &size, ui::progress_fn(ui, "Flash cache", path::base_name(file)), &err);
    const uint32_t ms = sys::now_ms() - t0;
    if (!p) {
        ui::message(ui, "Flash cache", {err ? err : "error"}, ui.theme().error);
        return;
    }
    uint32_t sum = 0;
    const uint32_t t1 = sys::now_ms();
    for (uint32_t i = 0; i < size; ++i) sum = sum * 31 + p[i];
    char a[64], b[64];
    snprintf(a, sizeof a, "%u bytes, loaded in %u ms", unsigned(size), unsigned(ms));
    snprintf(b, sizeof b, "checksum %08x (read in %u ms)", unsigned(sum), unsigned(sys::now_ms() - t1));
    ui::message(ui, "Flash cache", {a, b, "Load it again: it is instant."});
}

// ---------------------------------------------------------------- misc
void text_input(ui::Context& ui) {
    std::string name = "PicoCalc";
    if (!ui::prompt(ui, "Your name", name)) return;
    std::string pass;
    if (!ui::prompt(ui, "A secret", pass, nullptr, true)) return;
    const int r = ui::ask(ui, "Question", "Hello " + name + "! Do you like the ask() dialog?");
    ui::message(ui, "Result", {"Name: " + name, "Secret length: " + std::to_string(pass.size()),
                               std::string("Answer: ") + (r == 1 ? "yes" : r == 0 ? "no" : "cancelled")});
}

void system_info(ui::Context& ui) {
    char buf[8][64];
    snprintf(buf[0], 64, "Terminal: %s", g_term->name());
    snprintf(buf[1], 64, "Board: %s (%s)", board::name(), board::chip());
    snprintf(buf[2], 64, "Clock: %u MHz", unsigned(sys::clock_khz() / 1000));
    snprintf(buf[3], 64, "Flash: %u KB, cache %u KB", unsigned(board::flash_size() / 1024),
             unsigned(flash_cache::capacity() / 1024));
    snprintf(buf[4], 64, "Free heap: %u KB", unsigned(sys::free_heap() / 1024));
    const std::string bat = app::battery_text();
    snprintf(buf[5], 64, "Battery: %s", bat.empty() ? "unknown" : bat.c_str());
    uint64_t total = 0, free = 0;
    if (fs::space(&total, &free))
        snprintf(buf[6], 64, "SD: %u MB free of %u MB", unsigned(free >> 20), unsigned(total >> 20));
    else
        snprintf(buf[6], 64, "SD: not mounted");
    snprintf(buf[7], 64, "Wi-Fi hardware: %s", board::has_wifi() ? "yes" : "no");
    ui::message(ui, "System", {buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]});
}

}  // namespace

int main() {
    diag::begin("PicoPods demo", PD_APP_VERSION, kStages, array_count(kStages));
    TerminalConfig cfg;
    cfg.clock_khz = board::fast_clock_khz();
    Terminal& term = terminal_init(cfg);
    g_term = &term;
    gfx::init(term.display());
    diag::display_ready();
    diag::stage(1);

    ui::Context ui(term.keyboard(), ui::THEME_NIGHT);
    diag::stage(2);
    app::mount_storage(ui);
    flash_cache::init(2u * 1024 * 1024);
    diag::stage(3);
    diag::done();
#if PD_DEMO_WIFI
    WifiManager wifi("/.picopods/wifi.txt");
    fs::mkdirs("/.picopods");
    wifi.load();
    ui.idle = [&] { wifi.tick(); };   // keeps lwIP running and powers the radio down when idle
#endif

    std::string dir = "/";
    int theme = 0, last = 0;
    const char* const themes[] = {"Night", "Paper", "Phosphor"};
    for (;;) {
        std::vector<ui::MenuItem> items = {
            ui::action("Keyboard test"),
            ui::action("Fonts and colours"),
            ui::action("Blitter + audio"),
            ui::action("Text input"),
            ui::action("View a text file"),
            ui::action("Export text to ePub"),
            ui::action("Flash cache test"),
            ui::action("Wi-Fi"),
            ui::action("System info"),
            ui::choice("Theme", &theme, themes, 3),
            ui::action("Reboot to BOOTSEL"),
        };
#if !PD_DEMO_WIFI
        items[7].enabled = false;
#endif
        ui::MenuOptions opt;
        opt.initial = last;
        opt.footer = "PicoPods " PD_APP_VERSION;
        opt.on_change = [&](int) { ui.th = &ui::theme(theme); };
        app::splash(ui.theme(), "PicoPods", "library demo");
        const int r = ui::menu(ui, "DEMO", items, opt);
        if (r >= 0) last = r;
        switch (r) {
            case 0: keyboard_test(ui); break;
            case 1: fonts_and_colours(ui); break;
            case 2: plasma(ui); break;
            case 3: text_input(ui); break;
            case 4: view_file(ui, dir); break;
            case 5: export_epub(ui, dir); break;
            case 6: flash_test(ui, dir); break;
#if PD_DEMO_WIFI
            case 7: wifi.menu(ui); break;
#endif
            case 8: system_info(ui); break;
            case 10: sys::reboot_to_bootloader();
            default: break;
        }
    }
}
