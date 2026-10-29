// PicoPods - boot diagnostics and crash screen (PicoCalc Writer's diag.cpp,
// with app-defined stages; the fault hook also supports RP2040's Cortex-M0+).
#include "pd/diag.h"

#include <cstdarg>
#include <cstdio>

#include "pd/board.h"
#include "pd/gfx.h"
#include "pd/sys.h"

namespace pd::diag {
namespace {
constexpr uint32_t kMagic = 0xF3D10000u;
constexpr uint32_t kMagicMask = 0xFFFF0000u;
constexpr uint16_t kDone = 0xFFFF;
constexpr int kScratch = 3;   // not used by the SDK or by the PicoCalc UF2 Loader

const char* g_app = "PicoPods app";
const char* g_version = "";
const char* const* g_names = nullptr;
int g_count = 0;
int g_y = 0;
bool g_screen = false;
bool g_log = true;
int g_prev = -1;
uint32_t g_reset = 0;

const char* stage_name(int s) { return (g_names && s >= 0 && s < g_count) ? g_names[s] : "?"; }

void line(const char* s, Color fg) {
    printf("[diag] %s\n", s);
    if (!g_screen || !g_log) return;
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    if (g_y > gfx::height() - f.h) return;
    gfx::text(4, g_y, gfx::width() - 8, f, s, fg, 0x000000);
    g_y += f.h + 1;
}
}  // namespace

void begin(const char* app, const char* version, const char* const* names, int count) {
    g_app = app;
    g_version = version ? version : "";
    g_names = names;
    g_count = count;
    const uint32_t v = sys::scratch(kScratch);
    if ((v & kMagicMask) == kMagic) {
        const uint16_t st = uint16_t(v & 0xFFFF);
        if (st != kDone && st < count) g_prev = st;
    }
    g_reset = sys::reset_reason();
}

void display_ready() {
    g_screen = true;
    g_y = 4;
    char buf[80];
    snprintf(buf, sizeof buf, "%s %s  (%s, %s)", g_app, g_version, board::name(), board::chip());
    line(buf, rgb(255, 255, 255));
    snprintf(buf, sizeof buf, "reset reason 0x%08lx", (unsigned long)g_reset);
    line(buf, rgb(140, 140, 140));
    if (g_prev >= 0) {
        snprintf(buf, sizeof buf, "! previous start stopped at: %s", stage_name(g_prev));
        line(buf, rgb(255, 180, 0));
    }
}

void stage(int s) {
    sys::set_scratch(kScratch, kMagic | uint32_t(uint16_t(s)));
    char buf[64];
    snprintf(buf, sizeof buf, "%s...", stage_name(s));
    line(buf, rgb(120, 200, 120));
}

void note(const char* s) { line(s, rgb(200, 200, 200)); }
void done() { sys::set_scratch(kScratch, kMagic | kDone); }
void set_screen_log(bool on) { g_log = on; }
bool had_incomplete_start() { return g_prev >= 0; }
int previous_stage() { return g_prev; }

[[noreturn]] void crash_screen(const char* what, uint32_t pc, uint32_t lr, const char* msg) {
    printf("[diag] CRASH %s pc=%08lx lr=%08lx %s\n", what, (unsigned long)pc, (unsigned long)lr, msg ? msg : "");
    if (g_screen) {
        const Color bg = rgb(120, 0, 0), fg = rgb(255, 255, 255);
        const gfx::Font& f = gfx::font(gfx::FONT_6x12);
        const int w = gfx::width() - 16;
        gfx::fill(0, 0, gfx::width(), gfx::height(), bg);
        char buf[80];
        snprintf(buf, sizeof buf, "%s crashed", g_app);
        gfx::text(8, 8, w, f, buf, fg, bg);
        gfx::text(8, 24, w, f, what, fg, bg);
        snprintf(buf, sizeof buf, "pc=%08lx lr=%08lx", (unsigned long)pc, (unsigned long)lr);
        gfx::text(8, 40, w, f, buf, fg, bg);
        if (msg) gfx::text(8, 56, w, f, msg, fg, bg);
        const uint32_t v = sys::scratch(kScratch);
        if ((v & kMagicMask) == kMagic && int(v & 0xFFFF) < g_count) {
            snprintf(buf, sizeof buf, "during: %s", stage_name(int(v & 0xFFFF)));
            gfx::text(8, 72, w, f, buf, fg, bg);
        }
        gfx::text(8, 100, w, f, "Please send a photo of this screen.", fg, bg);
        gfx::text(8, 116, w, f, "Power-cycle to restart.", fg, bg);
    }
    for (;;) sys::sleep_ms(1000);
}

}  // namespace pd::diag

// ---------------------------------------------------------------- fault hooks
#if !PD_HOST
extern "C" {

void pd_hardfault_c(uint32_t* frame) {
    // Exception frame: r0 r1 r2 r3 r12 lr pc xpsr
    pd::diag::crash_screen("HardFault", frame[6], frame[5], nullptr);
}

#if PD_ARCH_ARM
// Overrides the SDK's weak default handler.
__attribute__((naked)) void isr_hardfault(void) {
#if PD_CHIP_RP2350
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b pd_hardfault_c\n");
#else   // Cortex-M0+: no IT blocks
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "ldr r1, =pd_hardfault_c\n"
        "bx r1\n"
        "1:\n"
        "mrs r0, msp\n"
        "ldr r1, =pd_hardfault_c\n"
        "bx r1\n");
#endif
}
#endif

// Installed through PICO_PANIC_FUNCTION=pd_panic (set by pd_diag).
void __attribute__((noreturn)) pd_panic(const char* fmt, ...) {
    char msg[64] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
    }
    pd::diag::crash_screen("panic", 0, uint32_t(uintptr_t(__builtin_return_address(0))), msg);
}

}  // extern "C"
#endif
