# Porting PicoPods

## To another board (MCU module)

Most Pico-format boards work without code changes:

```sh
cmake -B build -DPD_BOARD=pico          # RP2040
cmake -B build -DPD_BOARD=pico_w        # RP2040 + Wi-Fi
cmake -B build -DPD_BOARD=pico2         # RP2350
cmake -B build -DPD_BOARD=pimoroni_pico_plus2_rp2350   # any board the Pico SDK knows
```

What adapts automatically, from `pd/board.h`:

- **Chip** (`PD_CHIP_RP2040` / `PD_CHIP_RP2350`): reset-reason register, the flash cache's UF2 Loader partition detection (RP2350 only), the HardFault handler (Cortex-M0+ vs M33; none on RISC-V).
- **Wi-Fi** (`PD_HAS_WIFI`, and `PD_HAS_WIFI` in CMake): `pd_net` gets its CYW43 backend only on W boards.
- **Clocks**: `board::default_clock_khz()` from the SDK; `set_clock_khz()` raises the core voltage above the default speed.

If a board is not in the Pico SDK, write a board header as the SDK documents (`PICO_BOARD_HEADER_DIRS`) and pass its name to `PD_BOARD`. If it routes the terminal's signals to other GPIOs (an adapter board), override the pins, see below.

## Changing pins without a new terminal

Every PicoCalc pin is a macro in `lib/hardware/terminals/picocalc/include/pd/terminals/picocalc.h` and can be overridden per app:

```cmake
target_compile_definitions(my_app PRIVATE
    PD_PICOCALC_AUDIO_L=26        # older mainboards
    PD_PICOCALC_LCD_INVERT=0      # panels that show negative colours
    PD_PICOCALC_KBD_GAP_US=8000)  # snappier keyboard, if your firmware copes
```

## To another terminal

A terminal is one library that implements `pd::terminal_init()` and `pd::terminal()` (declared in `lib/hardware/terminals/include/pd/terminal.h`). Steps:

1. **Create** `lib/hardware/terminals/<name>/` with `include/pd/terminals/<name>.h` (pin map), `src/<name>.cpp`, `CMakeLists.txt` defining `pd_terminal_<name>`, and add it to `lib/hardware/terminals/CMakeLists.txt`.
2. **Reuse or write drivers.** A driver implements one HAL interface and takes its pins in a `Config`. For a chip PicoPods does not know (an ST7789 panel, a GPIO matrix keyboard, I2S audio), add it to `lib/hardware/drivers/`; only the driver talks to the SDK.
3. **Translate key codes** to `pd/keys.h` in the keyboard driver (these are the PicoCalc's codes; ASCII keys use ASCII).
4. **Wire it up** in `terminal_init()`: set the clock first, then initialise the drivers, fill in the `Terminal` fields, call `fs::set_block_device()` for storage and `keyboard_.attach()` for the keyboard.
5. **Build** with `-DPD_TERMINAL=<name>` and run `pd_demo`.

A skeleton:

```cpp
// lib/hardware/terminals/mydevice/src/mydevice.cpp
#include "pd/terminal.h"
#include "pd/fs.h"
#include "pd/sys.h"
#include "pd/drivers/st7789.h"      // your new display driver
#include "pd/drivers/gpio_matrix.h" // your new keyboard driver
#include "pd/drivers/sd_spi.h"      // reused
#include "pico/stdlib.h"

namespace pd {
namespace {
drivers::St7789 g_lcd;
drivers::GpioMatrix g_keys;
drivers::SdSpi g_sd;
Terminal g_term;
}

Terminal& terminal() { return g_term; }

Terminal& terminal_init(const TerminalConfig& cfg) {
    sys::set_clock_khz(cfg.clock_khz);
    if (cfg.debug_uart) stdio_init_all();
    g_lcd.init({/* spi, pins, 240x320 */});
    g_keys.init({/* rows, cols */});
    g_term.name_ = "MyDevice";
    g_term.display_ = &g_lcd;
    g_term.keyboard_device_ = &g_keys;
    g_term.keyboard_.attach(&g_keys);
    if (cfg.storage) {
        g_sd.configure({/* spi, pins */});
        g_term.storage_ = &g_sd;
        fs::set_block_device(&g_sd);
    }
    if (cfg.keyboard_on_core1) g_term.keyboard_.start_background();
    return g_term;
}
}
```

Things to check on a new terminal:

- **Screen size.** `pd_ui` lays out panels from `gfx::width()/height()` and the font, and `IndexedBlitter::begin` refuses sizes that do not fit. Test the demo's screens; very small screens may want `ui::Context::font_id = FONT_6x12`.
- **Pixel format.** Return the right `PixelFormat`; `encode()` does the rest. For RGB565 panels `bpp()` is 2 and all buffers shrink.
- **Slow keyboards.** Return a sensible `poll_interval_ms()`; use `keyboard_on_core1` if reads block.
- **No battery, no audio, no SD.** Leave those pointers `nullptr`; `app::battery_text()`, the status bar and the demo cope.

## The host terminal

`lib/hardware/terminals/host` is also a working example of a terminal: an RGB888 framebuffer display, a scripted keyboard, a paced null audio device, and the POSIX file system. It is what the tests run on. See `lib/hardware/terminals/host/include/pd/terminals/host.h` for the key-script syntax, e.g.

```sh
PD_SDCARD=./sd PD_FRAMES=./frames PD_KEYS="down,enter,text:hello,enter,esc" ./build-host/examples/demo/pd_demo
```

### Interactive window

When SDL2 is installed (`-DPD_HOST_SDL=AUTO`, the default; `ON` makes it
required, `OFF` leaves it out), every pod also gets a window: the PicoCalc
screen scaled up, the PC keyboard as the PicoCalc keyboard, and real sound.
It opens when `PD_KEYS` is not set, so tests and scripts stay headless:

```sh
PD_SDCARD=./sd ./build-host/pods/podwriter/podwriter          # window (carbo-pods)
PD_SCALE=3 PD_SDCARD=./sd ./build-host/pods/podboy/podboy     # bigger window (aurum-pods)
PD_WINDOW=1 PD_KEYS="down,enter" ./build-host/examples/demo/pd_demo   # play a script, then stay open
PD_WINDOW=0 ./build-host/examples/demo/pd_demo                # headless, keys from stdin
```

- **Keys.** Arrows, Enter, Esc, Backspace, Tab, Del, Insert, Home, End,
  PgUp/PgDn, F1–F10, Shift, Ctrl, left Alt and Caps Lock map to the PicoCalc
  keys of the same name, Pause to Break. Text keys type what your layout
  types (Shift+a is `A`, AltGr works). Only ASCII, like the PicoCalc.
- **F12** saves a screenshot as PPM (in `PD_FRAMES`, or the current folder).
- **Threads.** The window needs the main thread, so in these builds
  `picopods_add_pod()` renames the pod's `main()` to `pd_host_app_main` and
  runs it on a second thread. `main()` must therefore end with a `return`
  (the compiler warns otherwise).
- **No display** (SSH, CI): the pod runs headless as if `PD_WINDOW=0`. No
  sound card: it runs silent, still paced in real time.

What it does not show: anything below the HAL. The display, SD card, south
bridge and audio drivers, SPI clocks and real timing only run on the device.

### In the browser

The host terminal also builds with Emscripten (`tools/build_web_pods.sh` in
every repository for its own pods, `tools/build_web.sh` in aurum-pods for the
whole site; needs
the emsdk and `PICO_SDK_PATH`). Each pod becomes one self-contained file,
`<pod>.pod.js`, that `web/index.html` (aurum-pods) loads; the page is the launcher and the
runner (screen, touch keyboard, SD card panel, pod log).

- **One thread.** A page cannot block, but pods are written as endless loops.
  The pods are linked with Asyncify, and in the browser `pd::sys::sleep_*()`
  and `now_us()` hand control back to the page about every 8 ms (a message
  channel resumes the pod at once). Drawing, keys and sound happen in those
  gaps. A loop that never sleeps nor reads the clock would freeze the page.
  `launch_core1()` returns false, so the keyboard is polled.
- **Front end:** `lib/hardware/terminals/host/src/host_web.cpp` exports the
  framebuffer and a key entry point to the page and sends audio to Web Audio
  (`web/app.js`). The keyboard (`web/keyboard.js`) sends the same press /
  release codes as the PicoCalc; Shift, Ctrl and Alt are sticky on the touch
  keyboard (tap once for the next key, twice to lock), and a PC keyboard works
  too.
- **SD card:** `/sd`, an IndexedDB folder shared by every pod in that browser,
  loaded before `main()` and saved every 2 s and when the page is hidden.
- **Not there:** `pd_net` uses its "No Wi-Fi" stub (no sockets in a page);
  `pd_tls` compiles the Pico SDK's mbedTLS only so that Pod Files links.
  Bluetooth is absent.
