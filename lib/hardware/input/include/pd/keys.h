// PicoPods - common key codes.
//
// These are the codes the PicoCalc keyboard controller sends; PicoPods uses
// them on every terminal (other keyboard drivers translate to them). Printable
// ASCII keys use their ASCII code: Shift+A arrives as 'A'.
#pragma once
#include <cstdint>

namespace pd::key {

constexpr uint8_t Backspace = 0x08, Tab = 0x09, Enter = 0x0A, Space = 0x20;
constexpr uint8_t Esc = 0xB1;
constexpr uint8_t Left = 0xB4, Up = 0xB5, Down = 0xB6, Right = 0xB7;
constexpr uint8_t Break = 0xD0, Insert = 0xD1, Home = 0xD2, Del = 0xD4, End = 0xD5;
constexpr uint8_t PageUp = 0xD6, PageDown = 0xD7;
constexpr uint8_t F1 = 0x81, F2 = 0x82, F3 = 0x83, F4 = 0x84, F5 = 0x85;
constexpr uint8_t F6 = 0x86, F7 = 0x87, F8 = 0x88, F9 = 0x89, F10 = 0x90;
// Modifiers: tracked by pd::input::Keyboard, never returned by poll().
constexpr uint8_t Alt = 0xA1, ShiftL = 0xA2, ShiftR = 0xA3, Sym = 0xA4, Ctrl = 0xA5;
constexpr uint8_t CapsLock = 0xC1;
// Not a key: returned by ui::Context::wait_key() after background work drew
// over the screen (a dialog opened from idle()), so the waiting dialog redraws.
constexpr uint8_t Repaint = 0xFE;

constexpr bool is_function(uint8_t c) { return (c >= F1 && c <= F9) || c == F10; }
constexpr bool is_modifier(uint8_t c) {
    return c == Alt || c == ShiftL || c == ShiftR || c == Sym || c == Ctrl || c == CapsLock;
}
constexpr bool is_arrow(uint8_t c) { return c >= Left && c <= Right; }
constexpr bool is_printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }
constexpr uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? uint8_t(c + 32) : c; }

// Short human-readable name: "Enter", "F3", "Up", "A", "0xC5"... Returns a
// pointer into one of 4 rotating static buffers.
const char* name(uint8_t code);

}  // namespace pd::key
