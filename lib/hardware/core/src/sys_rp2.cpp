// pd::sys and pd::board on RP2040 / RP2350 (Pico SDK).
#include <malloc.h>

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pd/board.h"
#include "pd/clock_plan.h"
#include "pd/sys.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#if PD_CHIP_RP2350
#include "hardware/structs/powman.h"
#else
#include "hardware/structs/vreg_and_chip_reset.h"
#endif

extern "C" char __StackLimit, __end__;   // from the SDK linker scripts

namespace pd::sys {

uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }
uint64_t now_us() { return to_us_since_boot(get_absolute_time()); }
void sleep_ms(uint32_t ms) { ::sleep_ms(ms); }
void sleep_us(uint64_t us) { ::sleep_us(us); }

bool set_clock_khz(uint32_t khz) {
    if (khz == 0 || khz == clock_khz()) return true;
    uint vco = 0, div1 = 0, div2 = 0;
    if (!check_sys_clock_khz(khz, &vco, &div1, &div2)) return false;
    // Both chips are specified at 1.10 V up to their stock clock; 200 MHz needs
    // a little more. 1.15 V is what PicoBoy runs with on the PicoCalc.
    if (khz > board::default_clock_khz()) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
        ::sleep_ms(10);
    }
    if (!set_sys_clock_khz(khz, false)) return false;
    // The SDK parks clk_peri on the 48 MHz USB PLL when clk_sys changes
    // (PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=0), which caps every SPI
    // bus at 24 MHz. Feed it from clk_sys again, kept at or below the stock
    // clock: the RP2350 can divide it (200 MHz -> 100 MHz, so the LCD gets an
    // exact 50 MHz and the SD card 25 MHz); the RP2040 cannot.
    // SPI/I2C/UART dividers are recomputed by the drivers' init(), after this.
    const uint32_t hz = khz * 1000;
#if PD_CHIP_RP2350
    const uint32_t div = clock::peri_div(khz, board::default_clock_khz(), true);
    clock_configure_int_divider(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, hz, div);
#else
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, hz);
#endif
    return true;
}

uint32_t clock_khz() { return clock_get_hz(clk_sys) / 1000; }

namespace {
void (*g_core1_fn)() = nullptr;
void core1_entry() {
    flash_safe_execute_core_init();  // core 0 may pause us to write flash
    g_core1_fn();
}
}  // namespace

bool launch_core1(void (*fn)()) {
    if (g_core1_fn || !fn) return false;
    g_core1_fn = fn;
    multicore_launch_core1(core1_entry);
    return true;
}

void reboot() {
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void reboot_to_bootloader() {
    reset_usb_boot(0, 0);
    for (;;) tight_loop_contents();
}

uint32_t scratch(int i) { return (i >= 0 && i < 8) ? watchdog_hw->scratch[i] : 0; }
void set_scratch(int i, uint32_t v) {
    if (i >= 0 && i < 8) watchdog_hw->scratch[i] = v;
}

uint32_t reset_reason() {
#if PD_CHIP_RP2350
    return powman_hw->chip_reset;
#else
    return vreg_and_chip_reset_hw->chip_reset;
#endif
}

uint32_t free_heap() {
    const uint32_t total = uint32_t(&__StackLimit - &__end__);   // heap spans __end__..__StackLimit
    const struct mallinfo m = mallinfo();
    return total > uint32_t(m.uordblks) ? total - uint32_t(m.uordblks) : 0;
}

}  // namespace pd::sys

namespace pd::board {

#define PD_STR2(x) #x
#define PD_STR(x) PD_STR2(x)

const char* name() {
#ifdef PICO_BOARD
    return PICO_BOARD;
#else
    return "unknown";
#endif
}
const char* chip() { return PD_CHIP_RP2350 ? "RP2350" : "RP2040"; }
bool has_wifi() { return PD_HAS_WIFI; }
uint32_t flash_size() { return PICO_FLASH_SIZE_BYTES; }
uint32_t ram_size() { return PD_CHIP_RP2350 ? 520u * 1024 : 264u * 1024; }
uint32_t default_clock_khz() { return SYS_CLK_KHZ; }
uint32_t fast_clock_khz() { return 200000; }

}  // namespace pd::board
