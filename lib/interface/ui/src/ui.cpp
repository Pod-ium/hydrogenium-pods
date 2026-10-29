// PicoPods - menus and dialogs. Layout and behaviour follow picocalc_ui and
// PicoCalc Writer; geometry is derived from the display size and the font.
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "pd/fs.h"
#include "pd/path.h"
#include "pd/ui.h"

namespace pd::ui {

namespace {

using gfx::Font;
using input::KeyEvent;

// Panel geometry (on 320 px: panel 8..312, text 16..304, like picocalc_ui).
int W() { return gfx::width(); }
int H() { return gfx::height(); }
int panel_x() { return 8; }
int panel_w() { return W() - 16; }
int inner_x() { return 16; }
int inner_w() { return W() - 32; }
int inner_cells(const Context& ui) { return inner_w() / ui.font().w; }

void panel(Context& ui, int y, int h, const char* title) {
    const Theme& t = ui.theme();
    gfx::fill(panel_x(), y, panel_w(), h, t.panel_bg);
    gfx::frame(panel_x(), y, panel_w(), h, t.accent);
    if (title) gfx::text(inner_x(), y + 4, inner_w(), ui.font(), gfx::clip_cells(title, inner_cells(ui)), t.accent, t.panel_bg);
}

enum class Tone { Normal, Hi, Dim };

void line(Context& ui, int y, const std::string& s, Tone tone = Tone::Normal, Color color = kDefaultColor) {
    const Theme& t = ui.theme();
    Color fg = t.panel_fg, bg = t.panel_bg;
    if (tone == Tone::Hi) { fg = t.hi_fg; bg = t.hi_bg; }
    else if (tone == Tone::Dim) fg = t.dim;
    else if (color != kDefaultColor) fg = color;
    gfx::text(inner_x(), y, inner_w(), ui.font(), gfx::pad_cells(s, inner_cells(ui)), fg, bg);
}

// "label ........ right" in exactly `cells` characters.
std::string compose(const std::string& label, const std::string& right, int cells) {
    if (right.empty()) return gfx::pad_cells(label, cells);
    const int rc = gfx::text_cells(right) + 1;
    return gfx::pad_cells(label, cells - rc) + " " + right;
}

std::string item_text(const MenuItem& it, int cells) {
    char right[48] = {0};
    if (it.kind == MenuItem::CHOICE && it.value && it.count > 0) {
        int v = *it.value;
        if (v < 0 || v >= it.count) v = 0;
        snprintf(right, sizeof right, "< %s >", it.options[v]);
    } else if (it.kind == MenuItem::RANGE && it.value) {
        char num[24];
        snprintf(num, sizeof num, it.fmt ? it.fmt : "%d", *it.value);
        snprintf(right, sizeof right, "< %s >", num);
    } else if (it.hint) {
        snprintf(right, sizeof right, "%s", it.hint);
    }
    return compose(it.label, right, cells);
}

bool adjust(const MenuItem& it, int dir) {
    if (!it.value) return false;
    int v = *it.value;
    if (it.kind == MenuItem::CHOICE && it.count > 0) {
        v = ((v + dir) % it.count + it.count) % it.count;
    } else if (it.kind == MenuItem::RANGE) {
        v += dir * it.step;
        v = std::max(it.min, std::min(it.max, v));
    } else {
        return false;
    }
    if (v == *it.value) return false;
    *it.value = v;
    return true;
}

// Keep the end of a long path visible.
std::string clip_left(const std::string& s, int cells) {
    const int n = gfx::text_cells(s);
    if (n <= cells) return s;
    return ".." + s.substr(gfx::byte_of_cell(s, n - cells + 2));
}

}  // namespace

// ---------------------------------------------------------------- helpers
std::vector<std::string> wrap(const std::string& text, int cells) {
    std::vector<std::string> out;
    if (cells < 1) cells = 1;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (gfx::text_cells(para) > cells) {
            const size_t cut = gfx::byte_of_cell(para, cells);
            size_t sp = para.rfind(' ', cut);
            if (sp == std::string::npos || sp == 0) sp = cut;
            out.push_back(para.substr(0, sp));
            para = para.substr(sp < para.size() && para[sp] == ' ' ? sp + 1 : sp);
        }
        out.push_back(para);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string human_size(uint32_t n) {
    char b[16];
    if (n < 1024) snprintf(b, sizeof b, "%uB", unsigned(n));
    else if (n < 1024 * 1024) snprintf(b, sizeof b, "%uK", unsigned((n + 512) / 1024));
    else snprintf(b, sizeof b, "%u.%uM", unsigned(n >> 20), unsigned(((n & 0xFFFFF) * 10) >> 20));
    return b;
}

// ---------------------------------------------------------------- menu
MenuItem action(const char* label, const char* hint) {
    MenuItem m;
    m.kind = MenuItem::ACTION;
    m.label = label;
    m.hint = hint;
    return m;
}

MenuItem choice(const char* label, int* value, const char* const* options, int count) {
    MenuItem m;
    m.kind = MenuItem::CHOICE;
    m.label = label;
    m.value = value;
    m.options = options;
    m.count = count;
    return m;
}

MenuItem range(const char* label, int* value, int min, int max, int step, const char* fmt) {
    MenuItem m;
    m.kind = MenuItem::RANGE;
    m.label = label;
    m.value = value;
    m.min = min;
    m.max = max;
    m.step = step;
    m.fmt = fmt;
    return m;
}

MenuItem info(const char* label, const char* hint) {
    MenuItem m = action(label, hint);
    m.enabled = false;
    return m;
}

int menu(Context& ui, const char* title, const MenuItem* items, int n, const MenuOptions& opt) {
    if (n <= 0) return -1;
    const Font& f = ui.font();
    const int cells = inner_cells(ui);
    const int footer_h = opt.footer ? f.h : 0;
    const int max_rows = std::min(15, (H() - 12 - footer_h) / f.h - 1);
    const int visible = std::min(n, max_rows);
    const int h = (visible + 1) * f.h + 12 + footer_h;
    const int y0 = (H() - h) / 2;
    auto frame = [&]() {
        panel(ui, y0, h, title);
        if (opt.footer) line(ui, y0 + 8 + (visible + 1) * f.h, opt.footer, Tone::Dim);
    };
    frame();

    int sel = std::max(0, std::min(n - 1, opt.initial));
    for (int g = 0; g < n && !items[sel].enabled; ++g) sel = (sel + 1) % n;
    int top = 0;
    for (;;) {
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
        for (int i = 0; i < visible; ++i) {
            const MenuItem& it = items[top + i];
            const Tone tone = top + i == sel && it.enabled ? Tone::Hi : !it.enabled ? Tone::Dim : Tone::Normal;
            line(ui, y0 + 8 + (i + 1) * f.h, item_text(it, cells), tone);
        }
        const KeyEvent k = ui.wait_key();
        const MenuItem& cur = items[sel];
        if (k.code == key::Repaint) {
            frame();
        } else if (k.code == key::Up || k.code == key::Down) {
            const int d = k.code == key::Up ? -1 : 1;
            int s = sel;
            for (int g = 0; g < n; ++g) {
                s = (s + d + n) % n;
                if (items[s].enabled) break;
            }
            sel = s;
        } else if (k.code == key::PageUp || k.code == key::PageDown) {
            sel = k.code == key::PageUp ? 0 : n - 1;
            const int d = k.code == key::PageUp ? 1 : -1;
            for (int g = 0; g < n && !items[sel].enabled; ++g) sel = (sel + d + n) % n;
        } else if (k.code == key::Left || k.code == key::Right) {
            if (cur.enabled && adjust(cur, k.code == key::Right ? 1 : -1) && opt.on_change) opt.on_change(sel);
        } else if (k.code == key::Enter) {
            if (!cur.enabled) continue;
            if (cur.kind == MenuItem::ACTION) return sel;
            bool changed;
            if (cur.kind == MenuItem::RANGE && cur.value && *cur.value >= cur.max) {
                changed = *cur.value != cur.min;
                *cur.value = cur.min;   // wrap around
            } else {
                changed = adjust(cur, 1);
            }
            if (changed && opt.on_change) opt.on_change(sel);
        } else if (k.code == key::Esc || k.code == key::Backspace) {
            return -1;
        }
    }
}

// ---------------------------------------------------------------- dialogs
void message(Context& ui, const char* title, const std::vector<std::string>& in, Color color) {
    const Font& f = ui.font();
    std::vector<std::string> lines;
    for (const std::string& s : in)
        for (const std::string& w : wrap(s, inner_cells(ui))) lines.push_back(w);
    const int max_lines = std::min(15, (H() - 12) / f.h - 3);
    if (int(lines.size()) > max_lines) lines.resize(size_t(max_lines));
    const int h = (int(lines.size()) + 3) * f.h + 12;
    const int y0 = (H() - h) / 2;
    for (;;) {
        panel(ui, y0, h, title);
        for (size_t i = 0; i < lines.size(); ++i) line(ui, y0 + 8 + int(i + 1) * f.h, lines[i], Tone::Normal, color);
        line(ui, y0 + 8 + int(lines.size() + 2) * f.h, "Press any key", Tone::Dim);
        if (ui.wait_key().code != key::Repaint) return;
    }
}

int ask(Context& ui, const char* title, const std::string& text, bool allow_cancel) {
    const Font& f = ui.font();
    std::vector<std::string> lines = wrap(text, inner_cells(ui));
    if (lines.size() > 10) lines.resize(10);
    const int h = (int(lines.size()) + 3) * f.h + 12;
    const int y0 = (H() - h) / 2;
    auto draw = [&]() {
        panel(ui, y0, h, title);
        for (size_t i = 0; i < lines.size(); ++i) line(ui, y0 + 8 + int(i + 1) * f.h, lines[i]);
        line(ui, y0 + 8 + int(lines.size() + 2) * f.h, allow_cancel ? "[Y]es  [N]o  [Esc] cancel" : "[Y]es  [N]o",
             Tone::Dim);
    };
    draw();
    for (;;) {
        const KeyEvent k = ui.wait_key();
        if (k.code == key::Repaint) {
            draw();
            continue;
        }
        if (k.code == 'y' || k.code == 'Y' || k.code == key::Enter) return 1;
        if (k.code == 'n' || k.code == 'N') return 0;
        if (k.code == key::Esc || k.code == key::Backspace) return allow_cancel ? -1 : 0;
    }
}

bool confirm(Context& ui, const char* title, const std::string& question) {
    const Font& f = ui.font();
    std::vector<std::string> lines = wrap(question, inner_cells(ui));
    if (lines.size() > 10) lines.resize(10);
    const int h = (int(lines.size()) + 3) * f.h + 12;
    const int y0 = (H() - h) / 2;
    auto draw = [&]() {
        panel(ui, y0, h, title);
        for (size_t i = 0; i < lines.size(); ++i) line(ui, y0 + 8 + int(i + 1) * f.h, lines[i]);
        line(ui, y0 + 8 + int(lines.size() + 2) * f.h, "Enter: yes    Esc: no", Tone::Dim);
    };
    draw();
    for (;;) {
        const KeyEvent k = ui.wait_key();
        if (k.code == key::Repaint) {
            draw();
            continue;
        }
        if (k.code == key::Enter || k.code == 'y' || k.code == 'Y') return true;
        if (k.code == key::Esc || k.code == key::Backspace || k.code == 'n' || k.code == 'N') return false;
    }
}

bool prompt(Context& ui, const char* title, std::string& value, const char* hint, bool mask, size_t max_bytes) {
    const Theme& t = ui.theme();
    const Font& f = ui.font();
    const int h = 4 * f.h + 12;
    const int y0 = (H() - h) / 3;
    auto frame = [&]() {
        panel(ui, y0, h, title);
        line(ui, y0 + 8 + 3 * f.h - f.h / 2,
             hint ? hint : (mask ? "Enter OK  Tab show  Esc cancel" : "Enter = OK   Esc = cancel"), Tone::Dim);
    };
    frame();
    std::string s = value;
    size_t cur = s.size();
    int scroll = 0;
    const int cells = inner_cells(ui);
    bool reveal = false;
    for (;;) {
        const int cur_cell = gfx::text_cells(s.substr(0, cur));
        if (cur_cell < scroll) scroll = cur_cell;
        if (cur_cell - scroll >= cells - 1) scroll = cur_cell - cells + 2;
        const std::string shown = (mask && !reveal) ? std::string(size_t(gfx::text_cells(s)), '*') : s;
        const std::string vis = shown.substr(gfx::byte_of_cell(shown, scroll));
        gfx::text(inner_x(), y0 + 6 + int(1.5 * f.h), inner_w(), f, gfx::pad_cells(vis, cells), t.fg, t.bg,
                  cur_cell - scroll, t.cursor);
        const KeyEvent ev = ui.wait_key();
        if (ev.code == key::Repaint) {
            frame();
            continue;
        }
        if (ev.code == key::Enter) {
            value = s;
            return true;
        }
        if (ev.code == key::Esc) return false;
        if (mask && ev.code == key::Tab) {
            reveal = !reveal;
            continue;
        }
        if (ev.code == key::Left || ev.code == key::Backspace) {
            if (cur == 0) continue;
            size_t p = cur - 1;
            while (p > 0 && (uint8_t(s[p]) & 0xC0) == 0x80) --p;
            if (ev.code == key::Backspace) s.erase(p, cur - p);
            cur = p;
        } else if (ev.code == key::Right || ev.code == key::Del) {
            if (cur >= s.size()) continue;
            size_t p = cur + 1;
            while (p < s.size() && (uint8_t(s[p]) & 0xC0) == 0x80) ++p;
            if (ev.code == key::Del) s.erase(cur, p - cur);
            else cur = p;
        } else if (ev.code == key::Home || ev.code == key::Up) {
            cur = 0;
        } else if (ev.code == key::End || ev.code == key::Down) {
            cur = s.size();
        } else if (ev.is_ctrl('u')) {
            s.clear();
            cur = 0;
        } else if (char c = ev.ch()) {
            if (c == '\n' || c == '\t') continue;
            if (s.size() < max_bytes) s.insert(cur++, 1, c);
        }
    }
}

void progress(Context& ui, const char* title, const std::string& label, uint32_t done, uint32_t total) {
    const Font& f = ui.font();
    const Theme& t = ui.theme();
    const int h = 4 * f.h + 12;
    const int y0 = (H() - h) / 2;
    if (done == 0) panel(ui, y0, h, title);
    line(ui, y0 + 8 + f.h, label);
    const int bx = inner_x(), by = y0 + 8 + 3 * f.h, bw = inner_w(), bh = 10;
    const int filled = total ? int(uint64_t(std::min(done, total)) * uint64_t(bw - 4) / total) : 0;
    if (done == 0) {
        gfx::frame(bx, by, bw, bh, t.dim);
        gfx::fill(bx + 1, by + 1, bw - 2, bh - 2, t.panel_bg);
    }
    if (filled > 0) gfx::fill(bx + 2, by + 2, filled, bh - 4, t.accent);
}

void busy(Context& ui, const char* title, const std::string& label, bool cancellable) {
    const Font& f = ui.font();
    const int h = 4 * f.h + 12;
    const int y0 = (H() - h) / 2;
    panel(ui, y0, h, title);
    line(ui, y0 + 8 + f.h, label);
    if (cancellable) line(ui, y0 + 8 + 3 * f.h - f.h / 2, "Esc cancels", Tone::Dim);
}

std::function<bool(uint32_t, uint32_t)> progress_fn(Context& ui, const char* title, const std::string& label,
                                                    bool cancellable) {
    std::string t = title ? title : "";
    int last = -1;
    return [&ui, t, label, cancellable, last](uint32_t done, uint32_t total) mutable {
        const int pct = total ? int(uint64_t(std::min(done, total)) * 100 / total) : 0;
        if (last < 0) {
            progress(ui, t.c_str(), label, 0, total ? total : 1);
            last = 0;
        }
        if (pct != last) {
            progress(ui, t.c_str(), label, done ? done : 1, total ? total : 1);
            last = pct;
        }
        return !(cancellable && ui.kbd.esc_pressed());
    };
}

void text_view(Context& ui, const char* title, const std::vector<std::string>& in, int font_id) {
    const Theme& t = ui.theme();
    const Font& f = gfx::font(font_id);
    const int cells = (W() - 12) / f.w;
    std::vector<std::string> lines;
    for (const std::string& s : in)
        for (const std::string& w : wrap(s, cells)) lines.push_back(w);
    const int top_y = title ? f.h + 8 : 4;
    const int rows = (H() - top_y - f.h - 6) / f.h;
    const int n = int(lines.size());
    int top = 0;
    bool full = true;
    for (;;) {
        if (full) {
            gfx::fill(0, 0, W(), H(), t.panel_bg);
            if (title) gfx::text(6, 4, W() - 12, f, title, t.accent, t.panel_bg);
            full = false;
        }
        for (int i = 0; i < rows; ++i) {
            const int idx = top + i;
            gfx::text(6, top_y + i * f.h, W() - 12, f, gfx::pad_cells(idx < n ? lines[size_t(idx)] : "", cells),
                      t.panel_fg, t.panel_bg);
        }
        char foot[80];
        if (n > rows) snprintf(foot, sizeof foot, "%d-%d of %d   Esc close", top + 1, std::min(n, top + rows), n);
        else snprintf(foot, sizeof foot, "Esc close");
        gfx::text(6, H() - f.h - 4, W() - 12, f, gfx::pad_cells(foot, cells), t.dim, t.panel_bg);
        const KeyEvent k = ui.wait_key();
        const int max_top = std::max(0, n - rows);
        if (k.code == key::Repaint) {
            full = true;
            continue;
        }
        if (k.code == key::Esc || k.code == key::Enter || k.code == key::Backspace) return;
        if (k.code == key::Up) top = std::max(0, top - 1);
        else if (k.code == key::Down) top = std::min(max_top, top + 1);
        else if (k.code == key::PageUp || k.code == key::Left) top = std::max(0, top - rows);
        else if (k.code == key::PageDown || k.code == key::Right || k.code == key::Space) top = std::min(max_top, top + rows);
        else if (k.code == key::Home) top = 0;
        else if (k.code == key::End) top = max_top;
    }
}

// ---------------------------------------------------------------- browser
std::function<bool(const std::string&)> extension_filter(const char* exts) {
    std::vector<std::string> list;
    std::string cur;
    for (const char* p = exts;; ++p) {
        if (*p == ',' || *p == 0) {
            if (!cur.empty()) list.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else if (*p != ' ') {
            cur += *p;
        }
    }
    return [list](const std::string& name) {
        for (const std::string& e : list)
            if (path::has_extension(name, e.c_str())) return true;
        return false;
    };
}

bool browse(Context& ui, const char* title, std::string& dir, std::string& out, const BrowseOptions& opt) {
    const Theme& t = ui.theme();
    const Font& f = ui.font();
    const int cells = inner_cells(ui);
    const int rows = (H() - 8) / f.h - 2;
    std::string want = opt.select;

    for (;;) {
        if (!fs::is_dir(dir.c_str())) dir = "/";
        std::vector<fs::DirEntry> entries;
        if (!fs::list_dir(dir.c_str(), entries, opt.max_entries, opt.show_hidden)) {
            if (dir == "/") {
                message(ui, "Error", {"Cannot read the SD card.", fs::last_error_text()}, t.error);
                return false;
            }
            dir = "/";
            continue;
        }
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const fs::DirEntry& e) {
                                         for (const std::string& h : opt.hide)
                                             if (e.name == h) return true;
                                         return !e.is_dir && opt.filter && !opt.filter(e.name);
                                     }),
                      entries.end());
        std::sort(entries.begin(), entries.end(), [](const fs::DirEntry& a, const fs::DirEntry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        if (dir != "/") entries.insert(entries.begin(), fs::DirEntry{"..", true, 0});

        gfx::fill(0, 0, W(), H(), t.panel_bg);
        gfx::frame(4, 2, W() - 8, H() - 4, t.accent);
        gfx::text(inner_x(), 6, inner_w(), f, gfx::pad_cells(clip_left(std::string(title) + " " + dir, cells), cells),
                  t.accent, t.panel_bg);
        gfx::text(inner_x(), H() - f.h - 6, inner_w(), f,
                  gfx::pad_cells(opt.allow_cancel ? "Enter open  Bksp up  Esc cancel" : "Enter open  Bksp up", cells),
                  t.dim, t.panel_bg);

        const int n = int(entries.size());
        int sel = 0, top = 0;
        if (!want.empty()) {
            for (int i = 0; i < n; ++i)
                if (entries[size_t(i)].name == want) { sel = i; break; }
            want.clear();
        }
        bool redraw_all = true;
        int drawn_sel = -1, drawn_top = -1;

        auto draw_row = [&](int i) {
            const int idx = top + i;
            std::string s;
            if (idx < n) {
                const fs::DirEntry& e = entries[size_t(idx)];
                std::string right;
                if (e.is_dir) right = e.name == ".." ? "" : "<dir>";
                else if (opt.show_size) right = human_size(e.size);
                s = compose(e.name + (e.is_dir && e.name != ".." ? "/" : ""), right, cells);
            } else if (idx == 0) {
                s = opt.empty_hint ? opt.empty_hint : "(empty folder)";
            }
            const bool hi = idx == sel && idx < n;
            gfx::text(inner_x(), 6 + (i + 1) * f.h + 2, inner_w(), f, gfx::pad_cells(s, cells),
                      hi ? t.hi_fg : (idx < n ? t.panel_fg : t.dim), hi ? t.hi_bg : t.panel_bg);
        };

        bool reload = false;
        while (!reload) {
            if (sel < top) top = sel;
            if (sel >= top + rows) top = sel - rows + 1;
            if (redraw_all || top != drawn_top) {
                for (int i = 0; i < rows; ++i) draw_row(i);
                redraw_all = false;
            } else if (sel != drawn_sel) {
                draw_row(drawn_sel - top);
                draw_row(sel - top);
            }
            drawn_sel = sel;
            drawn_top = top;

            const KeyEvent k = ui.wait_key();
            if (k.code == key::Repaint) {   // full redraw: re-list in place
                if (sel < n) want = entries[size_t(sel)].name;
                reload = true;
                continue;
            }
            if (k.code == key::Esc && opt.allow_cancel) return false;
            if (k.code == key::Backspace || k.code == key::Left) {
                if (dir != "/") {
                    want = path::base_name(dir);
                    dir = path::parent(dir);
                    reload = true;
                }
                continue;
            }
            if (n == 0) continue;
            if (k.code == key::Up) sel = (sel + n - 1) % n;
            else if (k.code == key::Down) sel = (sel + 1) % n;
            else if (k.code == key::PageUp) sel = std::max(0, sel - rows);
            else if (k.code == key::PageDown) sel = std::min(n - 1, sel + rows);
            else if (k.code == key::Enter || k.code == key::Right) {
                const fs::DirEntry& e = entries[size_t(sel)];
                if (e.name == "..") {
                    want = path::base_name(dir);
                    dir = path::parent(dir);
                    reload = true;
                } else if (e.is_dir) {
                    dir = path::join(dir, e.name);
                    reload = true;
                } else if (k.code == key::Enter) {
                    out = path::join(dir, e.name);
                    return true;
                }
            }
        }
    }
}

}  // namespace pd::ui
