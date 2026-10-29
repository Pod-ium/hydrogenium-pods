// PicoPods - keyboard layers (raw events, key state, modifiers, auto-repeat).
// The cooked layer follows PicoCalc Writer's keyboard; the background queue
// follows PicoBoy's core-1 reader.
#include "pd/input.h"

#include <cstdio>

#include "pd/sys.h"

namespace pd::key {
const char* name(uint8_t k) {
    static char buf[4][8];
    static int slot = 0;
    switch (k) {
        case Enter: return "Enter";
        case Esc: return "Esc";
        case Backspace: return "Bksp";
        case Space: return "Space";
        case Tab: return "Tab";
        case Up: return "Up";
        case Down: return "Down";
        case Left: return "Left";
        case Right: return "Right";
        case Home: return "Home";
        case End: return "End";
        case Del: return "Del";
        case Insert: return "Ins";
        case PageUp: return "PgUp";
        case PageDown: return "PgDn";
        case Break: return "Break";
        case Ctrl: return "Ctrl";
        case Alt: return "Alt";
        case ShiftL: return "LShift";
        case ShiftR: return "RShift";
        case Sym: return "Sym";
        case CapsLock: return "Caps";
        default: break;
    }
    char* b = buf[slot++ & 3];
    if (k >= F1 && k <= F9) snprintf(b, 8, "F%d", k - 0x80);
    else if (k == F10) snprintf(b, 8, "F10");
    else if (k >= 'a' && k <= 'z') snprintf(b, 8, "%c", k - 'a' + 'A');
    else if (k > 32 && k < 127) snprintf(b, 8, "%c", k);
    else snprintf(b, 8, "0x%02X", k);
    return b;
}
}  // namespace pd::key

namespace pd::input {

namespace {
Keyboard* g_bg_owner = nullptr;

bool repeatable(uint8_t code, bool ctrl) {
    if (ctrl) return false;
    if (code == key::Esc || key::is_function(code)) return false;
    return true;
}
}  // namespace

// Single producer / single consumer ring.
bool Keyboard::Ring::push(const RawKey& e) {
    const uint32_t h = head.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1) & (kQueue - 1);
    if (next == tail.load(std::memory_order_acquire)) return false;  // full: drop
    ev[h] = e;
    head.store(next, std::memory_order_release);
    return true;
}

bool Keyboard::Ring::pop(RawKey& e) {
    const uint32_t t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire)) return false;
    e = ev[t];
    tail.store((t + 1) & (kQueue - 1), std::memory_order_release);
    return true;
}

void Keyboard::core1_loop() {
    Keyboard* k = g_bg_owner;
    for (;;) {
        RawKey ev;
        int n = 0;
        while (n++ < 8 && k->dev_->read(ev)) {
            if (ev.state < hal::KEY_PRESSED || ev.state > hal::KEY_RELEASED) continue;
            k->bgq_.push(ev);
        }
        sys::sleep_ms(1);
    }
}

bool Keyboard::start_background() {
    if (bg_) return true;
    if (!dev_ || g_bg_owner) return false;
    g_bg_owner = this;
    if (!sys::launch_core1(core1_loop)) {
        g_bg_owner = nullptr;
        return false;
    }
    bg_ = true;
    return true;
}

void Keyboard::inject(const RawKey& ev) { injected_.push(ev); }

bool Keyboard::next_raw(RawKey& ev) {
    if (injected_.pop(ev)) return true;
    if (!dev_) return false;
    if (bg_) return bgq_.pop(ev);
    const uint32_t t = sys::now_ms();
    if (t - last_poll_ < dev_->poll_interval_ms()) return false;
    for (int guard = 0; guard < 4; ++guard) {
        if (!dev_->read(ev)) {
            last_poll_ = t;   // queue empty: wait a little before asking again
            return false;
        }
        if (ev.state >= hal::KEY_PRESSED && ev.state <= hal::KEY_RELEASED) return true;
    }
    return false;
}

void Keyboard::track(const RawKey& ev) {
    const bool on = ev.state != hal::KEY_RELEASED;
    const uint32_t bit = 1u << (ev.code & 31);
    if (on) state_[ev.code >> 5] |= bit;
    else state_[ev.code >> 5] &= ~bit;
    switch (ev.code) {
        case key::Ctrl: mods_.ctrl = on; break;
        case key::Alt: mods_.alt = on; break;
        case key::ShiftL: shiftL_ = on; break;
        case key::ShiftR: shiftR_ = on; break;
        default: break;
    }
    mods_.shift = shiftL_ || shiftR_;
}

bool Keyboard::poll_raw(RawKey& ev) {
    if (!next_raw(ev)) return false;
    track(ev);
    if (ev.state == hal::KEY_RELEASED && ev.code == held_) held_ = 0;
    return true;
}

bool Keyboard::poll(KeyEvent& out) {
    RawKey ev;
    for (int guard = 0; guard < 16 && next_raw(ev); ++guard) {
        track(ev);
        if (key::is_modifier(ev.code)) continue;
        if (ev.state == hal::KEY_RELEASED) {
            if (ev.code == held_) held_ = 0;
            continue;
        }
        if (ev.state != hal::KEY_PRESSED) continue;   // "held": we repeat ourselves
        out = KeyEvent();
        out.code = ev.code;
        out.ctrl = mods_.ctrl;
        out.alt = mods_.alt;
        out.shift = mods_.shift;
        if (out.ctrl && ev.code >= 'A' && ev.code <= 'Z') out.code = uint8_t(ev.code - 'A' + 'a');
        held_ = repeatable(ev.code, mods_.ctrl) ? ev.code : 0;
        next_repeat_ = sys::now_ms() + cfg_.repeat_delay_ms;
        return true;
    }
    if (held_ && sys::reached(next_repeat_)) {
        out = KeyEvent();
        out.code = held_;
        out.ctrl = mods_.ctrl;
        out.alt = mods_.alt;
        out.shift = mods_.shift;
        out.repeat = true;
        next_repeat_ = sys::now_ms() + cfg_.repeat_rate_ms;
        return true;
    }
    return false;
}

KeyEvent Keyboard::wait() {
    KeyEvent ev;
    while (!poll(ev)) sys::sleep_ms(1);
    return ev;
}

bool Keyboard::wait_for(KeyEvent& ev, uint32_t timeout_ms) {
    const uint32_t until = sys::now_ms() + timeout_ms;
    for (;;) {
        if (poll(ev)) return true;
        if (sys::reached(until)) return false;
        sys::sleep_ms(1);
    }
}

void Keyboard::flush() {
    RawKey ev;
    while (next_raw(ev)) track(ev);
    held_ = 0;
}

bool Keyboard::esc_pressed() {
    RawKey ev;
    bool esc = false;
    for (int guard = 0; guard < 16 && next_raw(ev); ++guard) {
        track(ev);
        if (ev.code == key::Esc && ev.state == hal::KEY_PRESSED) esc = true;
    }
    return esc;
}

std::function<bool()> Keyboard::esc_checker() {
    return [this] { return !esc_pressed(); };
}

}  // namespace pd::input
