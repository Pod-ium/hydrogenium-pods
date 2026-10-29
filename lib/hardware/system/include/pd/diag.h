// PicoPods - start-up diagnostics and crash reporting (from PicoCalc Writer).
//
//   * an on-screen start-up log ("SD card...", "settings...")
//   * a breadcrumb in a watchdog scratch register that survives a reset, so
//     the next start can tell where the previous one stopped (and, for
//     example, skip reopening a file that crashed it: a "safe start")
//   * a red crash screen with the program counter for HardFaults and panics,
//     instead of a silent lock-up
//
// Linking pd_diag installs the HardFault handler and routes panic() to the
// crash screen.
//
//   static const char* const kStages[] = {"start", "display", "SD card", "settings"};
//   enum { kStart, kDisplay, kSd, kSettings };
//   int main() {
//       pd::diag::begin("MyApp", "1.0", kStages, 4);
//       pd::Terminal& t = pd::terminal_init();
//       pd::gfx::init(t.display());
//       pd::diag::display_ready();
//       pd::diag::stage(kSd);  ...
//       pd::diag::done();
//   }
#pragma once
#include <cstdint>

namespace pd::diag {

// Call first in main(), before touching hardware.
void begin(const char* app_name, const char* version, const char* const* stage_names, int stage_count);
void display_ready();    // after gfx::init(): prints the header and the previous-start warning
void stage(int s);       // records a breadcrumb and logs a line
void note(const char* s);
void done();             // start-up finished; clears the breadcrumb
void set_screen_log(bool on);   // on by default

bool had_incomplete_start();
int previous_stage();    // where the previous start stopped, or -1

[[noreturn]] void crash_screen(const char* what, uint32_t pc, uint32_t lr, const char* msg);

}  // namespace pd::diag
