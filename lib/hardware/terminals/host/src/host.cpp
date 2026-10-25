// PicoPods terminal - host (PC) implementation.
#include "pd/terminals/host.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "pd/fs.h"
#include "pd/sys.h"

namespace pd {

namespace {

// Set by the interactive front end before terminal_init().
std::atomic<bool> g_interactive{false};

// ---------------------------------------------------------------- display
class HostDisplay : public hal::Display {
public:
    void init(int w, int h) {
        std::lock_guard<std::mutex> lock(m_);
        w_ = w;
        h_ = h;
        fb_.assign(size_t(w) * size_t(h) * 3, 0);
        ++gen_;
    }
    int width() const override { return w_; }
    int height() const override { return h_; }
    hal::PixelFormat format() const override { return hal::PixelFormat::RGB888; }
    void begin(int x0, int y0, int x1, int y1) override {
        x0_ = x0; y0_ = y0; x1_ = x1; y1_ = y1;
        px_ = x0; py_ = y0; part_ = 0;
    }
    void write(const uint8_t* d, size_t n) override {
        // The lock only matters to the interactive window, which copies the
        // framebuffer from its own thread.
        std::lock_guard<std::mutex> lock(m_);
        ++gen_;
        for (size_t i = 0; i < n; ++i) {
            pix_[part_++] = d[i];
            if (part_ < 3) continue;
            part_ = 0;
            if (px_ >= 0 && py_ >= 0 && px_ < w_ && py_ < h_) memcpy(&fb_[(size_t(py_) * w_ + px_) * 3], pix_, 3);
            if (++px_ > x1_) {
                px_ = x0_;
                if (++py_ > y1_) py_ = y0_;
            }
        }
    }
    void end() override {}
    std::vector<uint8_t> fb_;
    int w_ = 320, h_ = 320;
    std::mutex m_;
    std::atomic<uint32_t> gen_{0};

private:
    int x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0, px_ = 0, py_ = 0, part_ = 0;
    uint8_t pix_[3] = {};
};

HostDisplay g_display;
int g_frame_no = 0;
std::string g_frames_dir;

void auto_dump() {
    if (g_frames_dir.empty()) return;
    char name[64];
    snprintf(name, sizeof name, "/f%03d.ppm", g_frame_no++);
    host::dump_frame(g_frames_dir + name);
}

// ---------------------------------------------------------------- keyboard
struct Step {
    std::vector<hal::RawKey> events;
    uint32_t wait_ms = 0;
    bool shot = false;
};

uint8_t named_key(const std::string& t) {
    static const struct { const char* n; uint8_t c; } names[] = {
        {"up", key::Up}, {"down", key::Down}, {"left", key::Left}, {"right", key::Right},
        {"enter", key::Enter}, {"esc", key::Esc}, {"bksp", key::Backspace}, {"backspace", key::Backspace},
        {"tab", key::Tab}, {"space", key::Space}, {"del", key::Del}, {"ins", key::Insert},
        {"home", key::Home}, {"end", key::End}, {"pgup", key::PageUp}, {"pgdn", key::PageDown},
        {"f1", key::F1}, {"f2", key::F2}, {"f3", key::F3}, {"f4", key::F4}, {"f5", key::F5},
        {"f6", key::F6}, {"f7", key::F7}, {"f8", key::F8}, {"f9", key::F9}, {"f10", key::F10},
        {"comma", ','}, {"break", key::Break},
    };
    if (t.size() == 1) return uint8_t(t[0]);
    std::string l = t;
    for (char& c : l) c = char(tolower(c));
    for (const auto& n : names)
        if (l == n.n) return n.c;
    if (l.size() > 2 && l[0] == '0' && l[1] == 'x') return uint8_t(strtoul(l.c_str() + 2, nullptr, 16));
    fprintf(stderr, "[host] unknown key token '%s'\n", t.c_str());
    return 0;
}

void tap(std::vector<hal::RawKey>& out, uint8_t code) {
    out.push_back({code, hal::KEY_PRESSED});
    out.push_back({code, hal::KEY_RELEASED});
}

Step parse_token(const std::string& tok) {
    Step s;
    if (tok.rfind("wait:", 0) == 0) {
        s.wait_ms = uint32_t(atoi(tok.c_str() + 5));
        return s;
    }
    if (tok == "shot") {
        s.shot = true;
        return s;
    }
    if (tok.rfind("text:", 0) == 0) {
        for (size_t i = 5; i < tok.size(); ++i) tap(s.events, uint8_t(tok[i]));
        return s;
    }
    std::string rest = tok;
    std::vector<uint8_t> mods;
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus == 0 || rest.size() == 1) break;
        std::string m = rest.substr(0, plus);
        for (char& c : m) c = char(tolower(c));
        if (m == "ctrl") mods.push_back(key::Ctrl);
        else if (m == "shift") mods.push_back(key::ShiftL);
        else if (m == "alt") mods.push_back(key::Alt);
        else break;
        rest = rest.substr(plus + 1);
    }
    const uint8_t code = named_key(rest);
    for (uint8_t m : mods) s.events.push_back({m, hal::KEY_PRESSED});
    if (code) tap(s.events, code);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) s.events.push_back({*it, hal::KEY_RELEASED});
    return s;
}

class HostKeyboard : public hal::KeyboardDevice {
public:
    void init() {
        const char* s = getenv("PD_KEYS");
        // Set but empty: no keys ever (tests inject their own). Interactive
        // without a script: only the window's live keys, no stdin.
        if ((s && !*s) || (!s && g_interactive)) {
            idle_ = true;
            return;
        }
        if (s) {
            script_ = true;
            std::string cur;
            for (const char* p = s;; ++p) {
                if (*p == ',' || *p == 0) {
                    // a lone ',' token would be empty: "comma" names it
                    if (!cur.empty()) steps_.push_back(parse_token(cur));
                    cur.clear();
                    if (!*p) break;
                } else {
                    cur += *p;
                }
            }
        }
    }
    bool read(hal::RawKey& ev) override {
        std::lock_guard<std::mutex> lock(m_);
        if (!live_.empty()) {
            ev = live_.front();
            live_.pop_front();
            return true;
        }
        if (idle_) return false;
        for (;;) {
            if (!pending_.empty()) {
                ev = pending_.front();
                pending_.pop_front();
                return true;
            }
            // Report "queue empty" once between tokens, like a real keyboard
            // between key strokes: flush() and esc_pressed() then stop here
            // instead of eating the rest of the script.
            if (!gap_) {
                gap_ = true;
                return false;
            }
            gap_ = false;
            if (!next_step()) return false;
        }
    }
    uint32_t poll_interval_ms() const override { return 1; }
    void push_live(hal::RawKey ev) {
        std::lock_guard<std::mutex> lock(m_);
        live_.push_back(ev);
    }

private:
    bool next_step() {
        if (wait_until_ && !sys::reached(wait_until_)) return false;
        wait_until_ = 0;
        if (script_) {
            if (idx_ >= steps_.size()) {
                auto_dump();
                fprintf(stderr, "[host] key script finished\n");
                if (g_interactive) {   // the window stays open for live keys
                    idle_ = true;
                    return false;
                }
                exit(0);
            }
            run(steps_[idx_++]);
            return true;
        }
        std::string tok;
        if (!(std::cin >> tok)) {
            auto_dump();
            exit(0);
        }
        run(parse_token(tok));
        return true;
    }
    void run(const Step& s) {
        if (s.wait_ms) {
            wait_until_ = sys::now_ms() + s.wait_ms;
            return;
        }
        auto_dump();
        (void)s.shot;
        for (const auto& e : s.events) pending_.push_back(e);
    }

    std::mutex m_;
    bool idle_ = false;
    bool gap_ = false;
    bool script_ = false;
    std::vector<Step> steps_;
    size_t idx_ = 0;
    std::deque<hal::RawKey> pending_;
    std::deque<hal::RawKey> live_;
    uint32_t wait_until_ = 0;
};

// ---------------------------------------------------------------- audio
// Paced in real time; with a sink (the interactive window) it also plays,
// and the sink's queue replaces the simulated one.
class HostAudio : public hal::AudioOut {
public:
    bool start(uint32_t rate) override {
        if (sink_ && !sink_->open(rate)) sink_ = nullptr;
        rate_ = rate;
        start_us_ = sys::now_us();
        consumed_base_ = 0;
        queued_ = 0;
        return true;
    }
    void stop() override {
        if (sink_) sink_->close();
        rate_ = 0;
    }
    uint32_t sample_rate() const override { return rate_; }
    void push(const int16_t* d, int frames) override {
        total_ += uint64_t(frames);
        if (sink_) {
            if (!rate_ || frames <= 0) return;
            const int room = kRing - sink_->queued();   // like the device's ring
            if (frames > room) frames = room > 0 ? room : 0;
            scaled_.resize(size_t(frames) * 2);
            for (size_t i = 0; i < scaled_.size(); ++i) scaled_[i] = int16_t(d[i] * vol_ / 100);
            sink_->queue(scaled_.data(), frames);
            return;
        }
        update();
        queued_ += uint64_t(frames);
        // Like the device's 2048-frame ring: only a real overflow loses audio.
        if (buffered() > kRing) queued_ = consumed_base_ + uint64_t(kRing);
    }
    int buffered() const override {
        if (sink_) return rate_ ? sink_->queued() : 0;
        const_cast<HostAudio*>(this)->update();
        return int(queued_ - consumed_base_);
    }
    int capacity() const override { return kRing - 700; }   // same as PwmAudio
    static constexpr int kRing = 2048;
    void silence() override {
        if (sink_) {
            sink_->clear();
            return;
        }
        update();
        queued_ = consumed_base_;
    }
    void set_volume(int v) override { vol_ = v < 0 ? 0 : v > 100 ? 100 : v; }
    int volume() const override { return vol_; }
    uint64_t total_ = 0;
    host::AudioSink* sink_ = nullptr;

private:
    // Frames "played" so far, at the real-time sample rate.
    void update() {
        if (!rate_) return;
        const uint64_t played = (sys::now_us() - start_us_) * rate_ / 1000000u;
        consumed_base_ = played < queued_ ? played : queued_;
        if (played > queued_) {   // underrun: restart the clock
            start_us_ = sys::now_us();
            queued_ = consumed_base_ = 0;
        }
    }
    uint32_t rate_ = 0;
    uint64_t start_us_ = 0, consumed_base_ = 0, queued_ = 0;
    int vol_ = 60;
    std::vector<int16_t> scaled_;
};

class HostPower : public hal::Power {
public:
    int battery_percent() override { return 73; }
    bool charging() override { return false; }
};

HostKeyboard g_kbd;
HostAudio g_audio;
HostPower g_power;
Terminal g_term;
bool g_ready = false;

}  // namespace

namespace host {

bool dump_frame(const std::string& p) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6 %d %d 255\n", g_display.w_, g_display.h_);
    fwrite(g_display.fb_.data(), 1, g_display.fb_.size(), f);
    fclose(f);
    return true;
}

const uint8_t* framebuffer() { return g_display.fb_.data(); }

Color pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_display.w_ || y >= g_display.h_) return 0;
    const uint8_t* p = &g_display.fb_[(size_t(y) * g_display.w_ + x) * 3];
    return rgb(p[0], p[1], p[2]);
}

uint64_t audio_frames_played() { return g_audio.total_; }

void push_key(hal::RawKey ev) { g_kbd.push_live(ev); }
void set_interactive(bool on) { g_interactive = on; }
void set_audio_sink(AudioSink* sink) { g_audio.sink_ = sink; }

uint32_t frame_generation() { return g_display.gen_; }

bool copy_frame(std::vector<uint8_t>& rgb, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_display.m_);
    if (g_display.fb_.empty()) return false;
    rgb = g_display.fb_;
    w = g_display.w_;
    h = g_display.h_;
    return true;
}

}  // namespace host

Terminal& terminal() { return g_term; }

Terminal& terminal_init(const TerminalConfig& cfg) {
    if (g_ready) return g_term;
    int w = 320, h = 320;
    if (const char* s = getenv("PD_SCREEN")) sscanf(s, "%dx%d", &w, &h);
    g_display.init(w, h);
    if (const char* d = getenv("PD_FRAMES")) {
        g_frames_dir = d;
        fs::set_root("");
        fs::mkdirs(d);
    }
    const char* sd = getenv("PD_SDCARD");
    fs::set_root(sd ? sd : "./sdcard");
    g_kbd.init();

    g_term.name_ = "host";
    g_term.display_ = &g_display;
    g_term.keyboard_device_ = &g_kbd;
    g_term.power_ = &g_power;
    g_term.keyboard_.attach(&g_kbd);
    if (cfg.storage) g_term.storage_ = nullptr;   // the host file system needs no block device
    if (cfg.audio) g_term.audio_ = &g_audio;
    // keyboard_on_core1 is ignored here: reading the host keyboard never
    // blocks, and a background reader would drain a key script at its own
    // pace instead of the app's (the script's frame dumps and waits would no
    // longer line up with what the app is doing).
    (void)cfg.keyboard_on_core1;
    g_ready = true;
    return g_term;
}

}  // namespace pd
