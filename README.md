# hydrogenium-pods

[![CI](https://github.com/Pod-ium/hydrogenium-pods/actions/workflows/ci.yml/badge.svg)](https://github.com/Pod-ium/hydrogenium-pods/actions/workflows/ci.yml)

The **PicoPods libraries** for the **ClockworkPi PicoCalc** with a Raspberry
Pi **Pico 2 W**, and the example pods that show how to build on them.

- [`lib/`](lib/README.md): the common libraries that drive the PicoCalc
  hardware (display, keyboard, SD card, audio, battery, Wi-Fi, Bluetooth,
  USB) and provide what every firmware needs (files, fonts, menus, dialogs,
  crash screen, editor engine);
- [`examples/`](examples): `pd_demo` (tour of the libraries and hardware
  check), `pd_template` (skeleton to copy for a new pod) and `pd_usbprobe`
  (check of the Pico's micro-USB port).

The firmwares built on them, the **pods**, live in their own repositories:

| Repository | Pods |
|---|---|
| [helium-pods](https://github.com/Pod-ium/helium-pods) | Games: **PodCraft** |
| [carbo-pods](https://github.com/Pod-ium/carbo-pods) | Productivity: **Pod Writer**, **Pod Files** |
| [aurum-pods](https://github.com/Pod-ium/aurum-pods) | Emulators: **PodBoy**, and **PicoPods Web** (every pod in the browser) |

They were all one tree, [Pod-ium/PicoPods](https://github.com/Pod-ium/PicoPods),
until it was split (see [`docs/ROADMAP.md`](docs/ROADMAP.md), M5).

## Build

```sh
# Examples for PicoCalc + Pico 2 W (default)
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j
# -> build/examples/<example>/<example>.uf2

# Other boards
cmake -B build-pico -DPD_BOARD=pico        # also pico_w, pico2, or any Pico SDK board name

# PC build: simulator + library tests
cmake -B build-host -DPD_TERMINAL=host -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build-host -j && ctest --test-dir build-host
# ...and with SDL2 installed, the demo runs in a window (keyboard, sound):
PD_SDCARD=./sd ./build-host/examples/demo/pd_demo

# The examples for the browser (.pod.js files, see aurum-pods for the page)
PICO_SDK_PATH=/path/to/pico-sdk tools/build_web_pods.sh   # -> dist/web-pods

# Both merge gates in one go
PICO_SDK_PATH=/path/to/pico-sdk tools/gate.sh all
```

Needs Pico SDK 2.1.x, `arm-none-eabi-gcc` and CMake ≥ 3.13. The host build
needs any C++17 compiler, Python 3, and `libmbedtls-dev` for the TLS/Git
tests; `PICO_SDK_PATH` there enables the Bluetooth tests (BTstack sources).
With `libsdl2-dev` (or SDL2 from Homebrew / MSYS2) it also builds the
interactive window, see [the host terminal](docs/PORTING.md#the-host-terminal).

Flash a `.uf2` in BOOTSEL mode, or run it from the SD card with
[UF2 Loader](https://github.com/pelrun/uf2loader): copy it to `pico2-apps/`
(Pico 2 / 2 W) or `pico1-apps/` (Pico / Pico W) and pick it in the loader's
menu. All pods and examples are plain Pico SDK images, which the loader runs
unchanged; the only code that writes flash (`pd_flash_cache`: PodBoy's ROM
cache, the demo's flash test) stays inside the app partition on the RP2350
and below the loader's top 16 KB on the RP2040. **First run on new
hardware:** flash `pd_demo.uf2` and go through its keyboard, colour, audio
and flash-cache screens.

## CI and releases

- **Pull requests and pushes** (`.github/workflows/ci.yml`): G-host (PC build
  + 19 library tests), G-fw (examples for `pico2_w`, `pico2`, `pico_w`,
  `pico`, `.uf2` files kept as artifacts `hydrogenium-<board>`) and G-web
  (the demo for the browser, artifact `hydrogenium-web-pods`).
- **Tags and releases** (`.github/workflows/release.yml`): pushing a tag
  `v*` creates its GitHub release, and publishing a release from the UI
  works too; either way `pd_demo_<version>.uf2`, `pd_usbprobe_<version>.uf2`
  and `SHA256SUMS_<version>.txt` are attached (tag `v1.2.0` gives version
  `1.2.0`). Locally: `PICO_SDK_PATH=… tools/package_release.sh <version>`.
  It can also be run by hand from the Actions tab for an existing tag.

The pod repositories build against this repository's `main` by default.
To pin them to a release of the libraries, tag it here and set their
repository variable `PICOPODS_REF` to the tag. Their CI also picks a branch
of this repository with the same name as theirs when there is one, so a
change that spans the libraries and a pod is tested together.

## Write a pod

In a pod repository, copy [`cmake/picopods_import.cmake`](cmake/picopods_import.cmake)
next to your top-level `CMakeLists.txt` (as you would `pico_sdk_import.cmake`):

```cmake
cmake_minimum_required(VERSION 3.13)
include(picopods_import.cmake)                    # before project(): board, terminal, SDK import
project(my_pods C CXX ASM)
include(${PICOPODS_PATH}/cmake/picopods.cmake)    # after project(): SDK init, libraries
add_subdirectory(pods/my_pod)
```

`picopods_import.cmake` uses `-DPICOPODS_PATH=/path/to/hydrogenium-pods` (or
the environment variable) when given, otherwise it fetches this repository
with git into the build tree (`PICOPODS_FETCH_FROM_GIT_TAG`, default `main`).
Then copy `examples/template` to `pods/my_pod` and rename it:

```cmake
picopods_add_pod(my_pod
    SOURCES main.cpp
    LIBS pd_diag pd_text          # anything beyond the core set
    TITLE "My Pod" VERSION "1.0")
```

```cpp
#include "pd/pd.h"

int main() {
    pd::TerminalConfig cfg;
    cfg.clock_khz = pd::board::fast_clock_khz();     // 200 MHz, optional
    pd::Terminal& t = pd::terminal_init(cfg);
    pd::gfx::init(t.display());
    pd::ui::Context ui(t.keyboard(), pd::ui::THEME_NIGHT);
    pd::app::mount_storage(ui);                      // "Insert an SD card" until it works

    int volume = 60;
    pd::ui::MenuItem items[] = {
        pd::ui::action("Open file"),
        pd::ui::range("Volume", &volume, 0, 100, 10, "%d%%"),
    };
    for (;;) {
        if (pd::ui::menu(ui, "MY POD", items) == 0) {
            std::string dir = "/", file;
            pd::ui::browse(ui, "Open", dir, file);
        }
    }
}
```

The pod repositories (helium-, carbo-, aurum-pods) are complete examples of
this layout, with their own `tools/` and CI workflows to copy.

## Layout

```
cmake/        picopods_init.cmake (before project), picopods.cmake (after project),
              picopods_import.cmake (copy into pod repositories)
lib/          common libraries, by layer: hardware/ services/ interface/ content/
examples/     demo/ template/ usbprobe/
tests/        library host tests (ctest) and their data
tools/        gate.sh (merge gates), package_release.sh (release .uf2), build_web_pods.sh
              (.pod.js), ci_ref.sh (CI: sibling repository ref), gen_fonts.py (font data)
docs/         ARCHITECTURE, PORTING, MIGRATION, ROADMAP
```

## Status

- Builds cleanly for `pico2_w`, `pico2`, `pico_w` and `pico` and for the
  host; CI checks all of them.
- Library host tests (19) pass: editor engine fuzz test with ASan/UBSan,
  syntax lexer, ePub samples with structure checks, X3 upload against a mock
  server, core libraries (files, settings, input, drawing, dialogs, blitter,
  flash cache), tree operations, the Bluetooth OBEX receiver, the BTstack
  lifecycle, and a scripted session of the demo. Git clones of real
  repositories run with `-DPD_NET_TESTS=ON` (needs network).
- New and untested on hardware: multi-block SD writes (they fall back to
  single-block writes on error) and the USB drive mode. The first hardware
  session should go through the demo.

See [`CHANGELOG.md`](CHANGELOG.md) for what changed from the original firmwares.
