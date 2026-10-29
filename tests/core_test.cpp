// Host tests for the PicoPods core libraries, run on the host terminal:
// settings, paths, fs, input layers, gfx rendering, ui dialogs (driven by
// injected key events), the indexed blitter and the flash cache.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pd/blit.h"
#include "pd/flash_cache.h"
#include "pd/pd.h"
#include "pd/terminals/host.h"
#include "pd/uf2loader.h"

using namespace pd;

static int g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void press(input::Keyboard& kb, uint8_t code) {
    kb.inject({code, hal::KEY_PRESSED});
    kb.inject({code, hal::KEY_RELEASED});
}

static void test_path_settings_fs() {
    fprintf(stderr, "[core] path_settings_fs\n");
    CHECK(path::join("/", "a") == "/a");
    CHECK(path::join("/a", "b.txt") == "/a/b.txt");
    CHECK(path::parent("/a/b") == "/a" && path::parent("/a") == "/");
    CHECK(path::stem("/r/Game.GB") == "Game" && path::extension("/r/Game.GB") == ".gb");
    CHECK(path::has_extension("x.Md", ".md") && !path::has_extension(".md", ".md"));
    CHECK(path::with_extension("/r/game.gb", ".sav") == "/r/game.sav");

    CHECK(fs::mount());
    CHECK(fs::mkdirs("/deep/er/folder") && fs::is_dir("/deep/er/folder"));
    Settings s;
    s.set_int("volume", 70);
    s.set("name", "multi\nline");
    s.set_bool("on", true);
    CHECK(s.save("/deep/settings.txt"));
    Settings r;
    CHECK(r.load("/deep/settings.txt"));
    CHECK(r.get_int("volume", 0, 0, 50) == 50);   // clamped
    CHECK(r.get("name") == "multi line" && r.get_bool("on", false) && !r.has("missing"));

    std::string back;
    CHECK(fs::write_file("/deep/hello.txt", "héllo") && fs::read_file("/deep/hello.txt", back) && back == "héllo");
    fs::Info info;
    CHECK(fs::stat("/deep/hello.txt", &info) && info.size == 6 && !info.is_dir);
    std::vector<fs::DirEntry> e;
    CHECK(fs::list_dir("/deep", e, 10) && e.size() == 3);
    fs::File f;
    CHECK(f.open("/deep/log.txt", fs::Mode::Append) && f.write("a", 1) == 1);
    f.close();
    CHECK(f.open("/deep/log.txt", fs::Mode::Append) && f.write("b", 1) == 1);
    f.close();
    CHECK(fs::read_file("/deep/log.txt", back) && back == "ab");
}

static void test_input(input::Keyboard& kb) {
    fprintf(stderr, "[core] input\n");
    kb.flush();
    input::KeyEvent ev;
    // Modifiers are attached to the next key, not reported themselves.
    kb.inject({key::Ctrl, hal::KEY_PRESSED});
    kb.inject({'S', hal::KEY_PRESSED});
    kb.inject({'S', hal::KEY_RELEASED});
    kb.inject({key::Ctrl, hal::KEY_RELEASED});
    CHECK(kb.poll(ev) && ev.is_ctrl('s') && ev.ch() == 0);
    CHECK(!kb.poll(ev));
    // Auto-repeat while held.
    kb.inject({'a', hal::KEY_PRESSED});
    CHECK(kb.poll(ev) && ev.code == 'a' && ev.ch() == 'a' && !ev.repeat);
    CHECK(kb.down('a'));
    sys::sleep_ms(520);
    CHECK(kb.poll(ev) && ev.code == 'a' && ev.repeat);
    kb.inject({'a', hal::KEY_RELEASED});
    CHECK(!kb.poll(ev) && !kb.down('a'));
    // Raw layer sees presses and releases.
    press(kb, key::Up);
    hal::RawKey rk;
    CHECK(kb.poll_raw(rk) && rk.code == key::Up && rk.state == hal::KEY_PRESSED);
    CHECK(kb.poll_raw(rk) && rk.state == hal::KEY_RELEASED);
    // Esc checker used by long operations.
    press(kb, 'x');
    press(kb, key::Esc);
    CHECK(kb.esc_pressed());
    CHECK(std::string(key::name(key::F3)) == "F3" && std::string(key::name('q')) == "Q");
}

static void test_gfx() {
    fprintf(stderr, "[core] gfx\n");
    gfx::clear(0x102030);
    CHECK(host::pixel(0, 0) == 0x102030 && host::pixel(319, 319) == 0x102030);
    gfx::fill(10, 10, 5, 5, 0xFF0000);
    CHECK(host::pixel(10, 10) == 0xFF0000 && host::pixel(14, 14) == 0xFF0000 && host::pixel(15, 15) == 0x102030);
    // A full block glyph does not exist; check that "|" draws some fg pixels
    // inside its cell and the band is filled with bg.
    const gfx::Font& f = gfx::font(gfx::FONT_8x16);
    const int n = gfx::text(0, 100, 40, f, "è|", 0xFFFFFF, 0x000000);
    CHECK(n == 2);
    int fg = 0;
    for (int y = 100; y < 116; ++y)
        for (int x = 8; x < 16; ++x) fg += host::pixel(x, y) == 0xFFFFFF;
    CHECK(fg > 4);
    CHECK(host::pixel(39, 100) == 0x000000);   // band beyond the text
    CHECK(gfx::text_cells("àèì") == 3 && gfx::pad_cells("ab", 4) == "ab  " && gfx::clip_cells("àèì", 2) == "àè");
    // Clipping at the screen edge must not crash.
    gfx::text(300, 310, 100, f, "clipped", 0xFFFFFF, 0);
    gfx::fill(-10, -10, 20, 20, 0x00FF00);
    CHECK(host::pixel(0, 0) == 0x00FF00);
}

static void test_ui(ui::Context& ui) {
    fprintf(stderr, "[core] ui\n");
    input::Keyboard& kb = ui.kbd;
    int volume = 50, pal = 0;
    const char* const names[] = {"A", "B", "C"};
    ui::MenuItem items[] = {
        ui::action("Resume"),
        ui::info("status line"),
        ui::range("Volume", &volume, 0, 100, 10),
        ui::choice("Palette", &pal, names, 3),
        ui::action("Quit"),
    };
    int changes = 0;
    ui::MenuOptions opt;
    opt.on_change = [&](int) { ++changes; };
    // Down skips the disabled line: lands on Volume. Right twice, Down, Left
    // (wraps to C), Down, Enter picks Quit.
    press(kb, key::Down);
    press(kb, key::Right);
    press(kb, key::Right);
    press(kb, key::Down);
    press(kb, key::Left);
    press(kb, key::Down);
    press(kb, key::Enter);
    CHECK(ui::menu(ui, "TEST", items, opt) == 4);
    CHECK(volume == 70 && pal == 2 && changes == 3);
    press(kb, key::Esc);
    CHECK(ui::menu(ui, "TEST", items) == -1);

    std::string s = "ab";
    kb.inject({key::Backspace, hal::KEY_PRESSED});
    kb.inject({key::Backspace, hal::KEY_RELEASED});
    for (char c : std::string("cd")) press(kb, uint8_t(c));
    press(kb, key::Home);
    press(kb, 'X');
    press(kb, key::Enter);
    CHECK(ui::prompt(ui, "Name", s) && s == "Xacd");

    press(kb, 'n');
    CHECK(ui::ask(ui, "Q", "Really?") == 0);
    press(kb, key::Esc);
    CHECK(ui::ask(ui, "Q", "Really?") == -1);
    press(kb, key::Enter);
    CHECK(ui::confirm(ui, "Q", "Sure?"));

    // Browser: /browse has a folder and two files, one filtered out.
    fs::mkdirs("/browse/sub");
    fs::write_file("/browse/b.gb", "x");
    fs::write_file("/browse/a.txt", "x");
    std::string dir = "/browse", out;
    ui::BrowseOptions bo;
    bo.filter = ui::extension_filter(".gb,.gbc");
    press(kb, key::Down);    // ".." -> sub/
    press(kb, key::Down);    // sub/ -> b.gb
    press(kb, key::Enter);
    CHECK(ui::browse(ui, "Pick", dir, out, bo) && out == "/browse/b.gb");
    press(kb, key::Down);
    press(kb, key::Enter);   // into sub/
    press(kb, key::Backspace);   // back up, preselects "sub"
    press(kb, key::Esc);
    CHECK(!ui::browse(ui, "Pick", dir, out, bo) && dir == "/browse");

    CHECK(ui::human_size(512) == "512B" && ui::human_size(2048) == "2K");
    const auto w = ui::wrap("one two three four", 10);
    CHECK(w.size() == 2 && w[0] == "one two" && w[1] == "three four");
}

static void test_blit() {
    fprintf(stderr, "[core] blit\n");
    static uint8_t fb[40 * 30];
    IndexedBlitter b;
    CHECK(b.begin(gfx::display(), {40, 30, 2, 10, 20}));
    const Color pal[2] = {0x000000, 0x00FFFF};
    b.set_palette(pal, 2);
    memset(fb, 0, sizeof fb);
    fb[5 * 40 + 7] = 1;
    CHECK(b.submit(fb));
    b.wait();
    CHECK(host::pixel(10 + 14, 20 + 10) == 0x00FFFF && host::pixel(10 + 15, 20 + 11) == 0x00FFFF);
    CHECK(host::pixel(10 + 16, 20 + 10) == 0x000000);
    gfx::fill(0, 0, 320, 320, 0xFF00FF);
    CHECK(b.submit(fb));   // unchanged frame: nothing is sent
    b.wait();
    CHECK(host::pixel(10 + 14, 20 + 10) == 0xFF00FF);
    b.invalidate();
    CHECK(b.submit(fb));
    b.wait();
    CHECK(host::pixel(10 + 14, 20 + 10) == 0x00FFFF);
    CHECK(!b.begin(gfx::display(), {200, 10, 2, 0, 0}));   // wider than the screen
}

static void test_flash_cache() {
    fprintf(stderr, "[core] flash_cache\n");
    std::string data(100000, 'q');
    data[99999] = 'Z';
    CHECK(fs::write_file("/rom.bin", data));
    CHECK(flash_cache::init(1 << 20));
    uint32_t size = 0;
    const char* err = nullptr;
    const uint8_t* p = flash_cache::load("/rom.bin", &size, nullptr, &err);
    CHECK(p && size == 100000 && p[99999] == 'Z');
    CHECK(flash_cache::load("/rom.bin", &size) == p);   // cached
    CHECK(!flash_cache::load("/missing.bin", &size, nullptr, &err) && err);
}

// RP2040 under UF2 Loader: the flash cache must end below the loader.
static void test_uf2loader() {
    fprintf(stderr, "[core] uf2loader\n");
    using namespace pd::uf2loader;
    constexpr uint32_t k2M = 2u * 1024 * 1024;
    // Marker as the loader writes it: an XIP address (2 MB - 8 KB loader - 8 KB BT).
    CHECK(rp2040_app_flash_end(k2M, kMagic, 0x101FC000) == k2M - 16 * 1024);
    // The same as a plain size (what the loader's README describes).
    CHECK(rp2040_app_flash_end(k2M, kMagic, 0x001F0000) == 0x001F0000);
    // No marker (BOOTSEL-flashed): keep the top 16 KB free anyway.
    CHECK(rp2040_app_flash_end(k2M, 0xFFFFFFFF, 0xFFFFFFFF) == k2M - 16 * 1024);
    // A marker pointing past the reserved area is not trusted.
    CHECK(rp2040_app_flash_end(k2M, kMagic, 0x10200000) == k2M - 16 * 1024);
    CHECK(rp2040_app_flash_end(k2M, kMagic, 0) == k2M - 16 * 1024);
}

int main() {
    if (const char* sd = getenv("PD_SDCARD")) (void)!system((std::string("rm -rf '") + sd + "'").c_str());
    TerminalConfig cfg;
    Terminal& t = terminal_init(cfg);
    CHECK(std::string(t.name()) == "host" && t.battery_percent() == 73);
    gfx::init(t.display());
    ui::Context ui(t.keyboard(), ui::THEME_NIGHT);
    test_path_settings_fs();
    test_input(t.keyboard());
    test_gfx();
    test_ui(ui);
    test_blit();
    test_flash_cache();
    test_uf2loader();
    host::dump_frame("core_test_last.ppm");
    printf("core tests passed (%d checks)\n", g_checks);
    return 0;
}
