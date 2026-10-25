// pd::sys and pd::board on a PC (host builds: simulator and unit tests).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pd/board.h"
#include "pd/sys.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pd::sys {
namespace {
const auto g_start = std::chrono::steady_clock::now();
uint32_t g_scratch[8] = {};

uint64_t clock_us() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now() - g_start).count());
}

#ifdef __EMSCRIPTEN__
// In a browser the pod runs on the page's only thread, and nothing (drawing,
// keys, sound) happens until it hands control back. Asyncify lets these calls
// pause the pod and resume it from the browser's event loop. A message
// channel resumes it at once (setTimeout would wait at least 4 ms).
EM_ASYNC_JS(void, pd_web_pause, (int ms), {
    if (ms > 0) return new Promise(r => setTimeout(r, ms));
    if (!Module.pdResume) {
        const ch = new MessageChannel();
        Module.pdResumeQueue = [];
        ch.port1.onmessage = () => Module.pdResumeQueue.shift()();
        Module.pdResume = r => { Module.pdResumeQueue.push(r); ch.port2.postMessage(0); };
    }
    return new Promise(r => Module.pdResume(r));
});

uint64_t g_last_yield = 0;
constexpr uint64_t kYieldEveryUs = 8000;   // about two yields per 60 Hz frame

void yield_to_browser(int ms) {
    pd_web_pause(ms);
    g_last_yield = clock_us();
}
#endif
}  // namespace

uint64_t now_us() {
    const uint64_t t = clock_us();
#ifdef __EMSCRIPTEN__
    // Pods wait in loops on the clock (PodBoy's frame pacing): let the page
    // breathe from here too, or a busy wait would freeze it.
    if (t - g_last_yield > kYieldEveryUs) yield_to_browser(0);
#endif
    return t;
}
uint32_t now_ms() { return uint32_t(now_us() / 1000); }
#ifdef __EMSCRIPTEN__
void sleep_us(uint64_t us) {
    const uint64_t until = clock_us() + us;
    if (us >= 2000) yield_to_browser(int(us / 1000));
    while (clock_us() < until) now_us();   // the rest, yielding as needed
}
void sleep_ms(uint32_t ms) { sleep_us(uint64_t(ms) * 1000); }
#else
void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void sleep_us(uint64_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
#endif

bool set_clock_khz(uint32_t) { return true; }
uint32_t clock_khz() { return 150000; }

bool launch_core1(void (*fn)()) {
#ifdef __EMSCRIPTEN__
    (void)fn;
    return false;   // one thread in the browser: callers poll instead
#endif
    static bool launched = false;
    if (launched || !fn) return false;
    launched = true;
    std::thread(fn).detach();
    return true;
}

void reboot() {
    fprintf(stderr, "[host] reboot requested\n");
    exit(0);
}
void reboot_to_bootloader() {
    fprintf(stderr, "[host] reboot to bootloader requested\n");
    exit(0);
}

uint32_t scratch(int i) { return (i >= 0 && i < 8) ? g_scratch[i] : 0; }
void set_scratch(int i, uint32_t v) {
    if (i >= 0 && i < 8) g_scratch[i] = v;
}
uint32_t reset_reason() { return 0; }
uint32_t free_heap() { return 256u * 1024; }

}  // namespace pd::sys

namespace pd::board {
const char* name() { return "host"; }
const char* chip() { return "host"; }
bool has_wifi() { return false; }
uint32_t flash_size() { return 4u * 1024 * 1024; }
uint32_t ram_size() { return 520u * 1024; }
uint32_t default_clock_khz() { return 150000; }
uint32_t fast_clock_khz() { return 200000; }
}  // namespace pd::board
