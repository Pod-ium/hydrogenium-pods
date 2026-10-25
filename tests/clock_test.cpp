// Host test for pd/clock_plan.h: the clk_peri that pd::sys::set_clock_khz()
// picks and the SPI rates the PicoCalc buses then really run at (issue #12:
// PodBoy's LCD, asked for 50 MHz, ran at 24 MHz after the overclock).
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "pd/clock_plan.h"

using namespace pd::clock;

static int g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

constexpr uint32_t kMHz = 1000000;
constexpr uint32_t kRp2350Stock = 150000;   // SYS_CLK_KHZ, Pico SDK 2.1
constexpr uint32_t kRp2040Stock = 125000;
constexpr uint32_t kFast = 200000;          // pd::board::fast_clock_khz()
// pd/terminals/picocalc.h defaults; PodBoy overrides the LCD to 50 MHz.
constexpr uint32_t kLcdHz = 25 * kMHz;
constexpr uint32_t kPodBoyLcdHz = 50 * kMHz;
constexpr uint32_t kSdHz = 25 * kMHz;

// The SDK's search, checked against hand-worked divisors.
static void test_spi_hz() {
    fprintf(stderr, "[clock] spi_hz\n");
    CHECK(spi_hz(100 * kMHz, 50 * kMHz) == 50 * kMHz);     // 2 * 1
    CHECK(spi_hz(100 * kMHz, 25 * kMHz) == 25 * kMHz);     // 2 * 2
    CHECK(spi_hz(150 * kMHz, 25 * kMHz) == 25 * kMHz);     // 2 * 3
    CHECK(spi_hz(150 * kMHz, 50 * kMHz) == 37500000);      // 2 * 2: never above the request
    CHECK(spi_hz(125 * kMHz, 400000) <= 400000);           // SD init clock
    CHECK(spi_hz(125 * kMHz, 400000) > 390000);
    CHECK(spi_hz(48 * kMHz, 100 * kMHz) == 24 * kMHz);     // faster than clk_peri: clk_peri / 2
    CHECK(spi_hz(150 * kMHz, 1000) == 0);                  // slower than 254 * 256 allows
    // Any request: at most what was asked, at most clk_peri / 2.
    for (uint32_t peri : {48 * kMHz, 100 * kMHz, 125 * kMHz, 150 * kMHz, 200 * kMHz})
        for (uint32_t want = kMHz; want <= peri; want += kMHz) {
            const uint32_t got = spi_hz(peri, want);
            CHECK(got > 0 && got <= want && got <= peri / 2);
        }
}

// Issue #12: with clk_peri left on the 48 MHz USB PLL every bus tops out at
// 24 MHz, so asking for 50 MHz changed nothing.
static void test_usb_pll_cap() {
    fprintf(stderr, "[clock] usb_pll_cap\n");
    CHECK(spi_hz(48 * kMHz, kLcdHz) == 24 * kMHz);
    CHECK(spi_hz(48 * kMHz, kPodBoyLcdHz) == 24 * kMHz);
}

static void test_peri_clock() {
    fprintf(stderr, "[clock] peri_clock\n");
    CHECK(peri_div(kFast, kRp2350Stock, true) == 2);
    CHECK(peri_hz(kFast, kRp2350Stock, true) == 100 * kMHz);
    CHECK(peri_hz(kRp2350Stock, kRp2350Stock, true) == 150 * kMHz);
    CHECK(peri_hz(kFast, kRp2040Stock, false) == 200 * kMHz);   // RP2040: no divider
    // RP2350: clk_peri never above stock, for any clock up to 3x stock.
    for (uint32_t khz = 48000; khz <= 3 * kRp2350Stock; khz += 1000) {
        const uint32_t div = peri_div(khz, kRp2350Stock, true);
        CHECK(div >= 1 && div <= kMaxPeriDiv);
        CHECK(peri_hz(khz, kRp2350Stock, true) <= kRp2350Stock * 1000);
    }
}

// What each firmware's buses really get. PodBoy and the demo switch to the
// fast clock; the other pods stay at stock (set_clock_khz() leaves clk_peri
// as the SDK boots it: clk_sys, undivided, which peri_hz() also gives).
static void test_picocalc_buses() {
    fprintf(stderr, "[clock] picocalc_buses\n");
    struct Case {
        const char* what;
        uint32_t stock_khz, sys_khz;
        bool rp2350;
        uint32_t lcd_hz;
    } cases[] = {
        {"podboy pico2", kRp2350Stock, kFast, true, kPodBoyLcdHz},
        {"podboy pico", kRp2040Stock, kFast, false, kPodBoyLcdHz},
        {"demo pico2", kRp2350Stock, kFast, true, kLcdHz},
        {"demo pico", kRp2040Stock, kFast, false, kLcdHz},
        {"stock pico2", kRp2350Stock, kRp2350Stock, true, kLcdHz},
    };
    for (const Case& c : cases) {
        const uint32_t peri = peri_hz(c.sys_khz, c.stock_khz, c.rp2350);
        const uint32_t lcd = spi_hz(peri, c.lcd_hz), sd = spi_hz(peri, kSdHz);
        fprintf(stderr, "  %-12s clk_peri %3u MHz  LCD %.1f MHz  SD %.1f MHz\n", c.what, peri / kMHz,
                lcd / 1e6, sd / 1e6);
        CHECK(lcd == c.lcd_hz);
        CHECK(sd == kSdHz);
    }
    // RP2040 at stock: 125 MHz has no even divisor giving 25 MHz exactly.
    CHECK(spi_hz(kRp2040Stock * 1000, kLcdHz) == 20833333);
}

int main() {
    test_spi_hz();
    test_usb_pll_cap();
    test_peri_clock();
    test_picocalc_buses();
    fprintf(stderr, "[clock] %d checks passed\n", g_checks);
    return 0;
}
