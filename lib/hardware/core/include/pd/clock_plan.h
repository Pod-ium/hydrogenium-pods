// PicoPods - clock arithmetic behind pd::sys::set_clock_khz() and the SPI
// buses that hang off clk_peri.
//
// - clk_peri: after an overclock, set_clock_khz() feeds clk_peri from clk_sys,
//   divided (RP2350, divider 1..3) so it stays at or below the stock clock.
//   The RP2040 has no divider and takes clk_sys as is.
// - SPI: spi_set_baudrate() picks an even prescale (2..254) and a post-divide
//   (1..256), so the rate a bus really gets is clk_peri / (prescale * postdiv),
//   at most clk_peri / 2, never above the request.
//
// Header only and free of SDK includes, so the rates every pod gets are
// host-tested (tests/clock_test.cpp): issue #12 was an LCD asked for 50 MHz
// that ran at 24 MHz because clk_peri had been left on the 48 MHz USB PLL.
#pragma once
#include <cstdint>

namespace pd::clock {

constexpr uint32_t kMaxPeriDiv = 3;   // RP2350 CLK_PERI_DIV

// Divider set_clock_khz() puts on clk_peri when clk_sys runs at sys_khz.
constexpr uint32_t peri_div(uint32_t sys_khz, uint32_t stock_khz, bool can_divide) {
    if (!can_divide || stock_khz == 0) return 1;
    uint32_t div = (sys_khz + stock_khz - 1) / stock_khz;
    if (div < 1) div = 1;
    if (div > kMaxPeriDiv) div = kMaxPeriDiv;
    return div;
}

// clk_peri in Hz after set_clock_khz(sys_khz).
constexpr uint32_t peri_hz(uint32_t sys_khz, uint32_t stock_khz, bool can_divide) {
    return sys_khz * 1000 / peri_div(sys_khz, stock_khz, can_divide);
}

// The rate spi_set_baudrate(spi, want_hz) gives with clk_peri at peri_hz
// (same search as Pico SDK 2.1's hardware_spi). A request above clk_peri / 2
// silently gets clk_peri / 2, as in the SDK with parameter assertions off
// (the default). 0 if want_hz is too slow to reach.
constexpr uint32_t spi_hz(uint32_t peri_hz, uint32_t want_hz) {
    if (want_hz == 0) return 0;
    uint32_t prescale = 2;
    for (; prescale <= 254; prescale += 2)
        if (uint64_t(peri_hz) < uint64_t(prescale) * 256 * want_hz) break;
    if (prescale > 254) return 0;
    uint32_t postdiv = 256;
    for (; postdiv > 1; --postdiv)
        if (peri_hz / (prescale * (postdiv - 1)) > want_hz) break;
    return peri_hz / (prescale * postdiv);
}

}  // namespace pd::clock
