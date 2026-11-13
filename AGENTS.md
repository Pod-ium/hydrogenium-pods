# AGENTS.md: hydrogenium-pods

Instructions for an AI agent that adds features, fixes bugs or writes
documentation **in this repository**. Read this file, then
`docs/ARCHITECTURE.md` and `lib/README.md`, before changing code.

## What this repository is

The **PicoPods libraries** for the ClockworkPi **PicoCalc** (Raspberry Pi
**Pico 2 W** by default; Pico, Pico W, Pico 2 also supported), the build
system every pod uses, three example pods and the library tests. It is the
foundation of the Pod-ium family:

| Repository | Content | Depends on this repo through |
|---|---|---|
| **hydrogenium-pods** (here) | `lib/`, `cmake/`, `examples/`, library tests, `docs/` | — |
| helium-pods | games (PodCraft) | `picopods_import.cmake` |
| carbo-pods | productivity (Pod Writer, Pod Files) | `picopods_import.cmake` |
| aurum-pods | emulators (PodBoy), PicoPods Web | `picopods_import.cmake` |

**The three pod repositories build against this repository's `main`.** A
change merged here is picked up by their next CI run. Treat the public API
(`lib/**/include/pd/*.h`, `cmake/*.cmake`, `picopods_add_pod()`) as a
contract with them.

## Layout

```
cmake/        picopods_init.cmake (before project()), picopods.cmake (after project()),
              picopods_import.cmake (the copy pod repositories carry)
lib/          libraries by layer: hardware/ services/ interface/ content/  (catalogue: lib/README.md)
examples/     demo/ (pd_demo: library tour + hardware check), template/ (pd_template), usbprobe/
tests/        library host tests (ctest), test data in tests/data/, Python checkers
tools/        gate.sh, package_release.sh, build_web_pods.sh, ci_ref.sh, ci_apt.sh, gen_fonts.py
docs/         ARCHITECTURE.md, PORTING.md, MIGRATION.md, ROADMAP.md
```

## What belongs here

- Anything touching hardware, drivers, terminals, the HAL, files, Wi-Fi,
  Bluetooth, TLS, Git, graphics, UI dialogs, the text engine, ePub, the build
  system or the host/browser terminals.
- Anything **more than one pod would use**. Pods must not carry private copies
  of library code; if a pod needs a generic facility, add it here.
- Pod-specific behaviour (screens, keys, saves of one pod) does **not** belong
  here: it goes in that pod's repository.

## Architecture rules (do not break)

- **Three axes stay separate:** board (`-DPD_BOARD`, `pd/board.h`), terminal
  (`-DPD_TERMINAL`, `lib/hardware/terminals/<name>`), pod (`picopods_add_pod`).
- Only `pd_drivers`, `pd_terminal_*`, `pd_core`, the device backends of
  `pd_net`, `pd_bt`, `pd_fs` (FatFs), `pd_usb_drive`, `pd_flash_cache` and
  `pd_diag` include Pico SDK headers. Everything else is plain **C++17** and
  must compile on the PC host and under Emscripten.
- **Pins exist only in the terminal** (`pd/terminals/picocalc.h`, as
  overridable `PD_PICOCALC_*` macros). Drivers take pins/buses in a `Config`.
- A layer only uses the layers above it in `lib/README.md`
  (hardware → services → interface → content).
- Libraries are CMake **`INTERFACE`** targets named `pd_<name>`, headers in
  `include/pd/`, namespace `pd::`. A new library gets a row in the right
  table of `lib/README.md`.
- **Backends per platform** live side by side (`*_cyw43.cpp`, `*_posix.cpp`,
  `*_none.cpp`, `*_sim.cpp`, `fs_fatfs.cpp` / `fs_posix.cpp`). A change to the
  behaviour of one backend usually needs the same change in the others.
- **Boards without Wi-Fi/BT** must still build: CMake sets `PD_HAS_WIFI`,
  `PD_HAS_TLS`, `PD_HAS_BT`; the `none` backends report "not available".
- **Memory:** 520 KB SRAM (RP2350), 264 KB (RP2040). No full framebuffer
  (`pd_gfx` streams one row; `IndexedBlitter` for indexed frames), large
  buffers allocated when a feature starts and sized from the real display,
  constant data in flash.
- **Waiting** goes through `ui::Context::wait_key()` / `Context::idle`, and
  sleeping through `pd::sys`, so Wi-Fi/audio/BT keep running and the browser
  build can yield (Asyncify).
- **Flash** is written only by `pd_flash_cache`, which stays inside UF2
  Loader's app partition (RP2350) and below its top 16 KB (RP2040).
- **Core 1** is used only when a pod asks (`keyboard_on_core1`,
  `sys::launch_core1()`).
- Third-party code (`fatfs/`, `miniz/`, the fonts) is vendored: don't edit it
  except to update it, and keep its licence file.

### Compatibility
- Keep existing `pd/*.h` signatures working. If a breaking change is
  unavoidable, coordinate it with the pod repositories (see "Changes that
  span repositories") and say so in the changelog.
- File formats on users' SD cards must keep loading: `pd::Settings`
  (`key=value`), saved Wi-Fi credentials, Bluetooth keys
  (`/.picopods/bt_keys.bin`), the text engine's work files.
- Outward-facing identifiers say PicoPods (user agent, USB vendor strings).

## Build and gates

Toolchain (as CI): Pico SDK **2.1.1** (`PICO_SDK_PATH`), Arm GNU Toolchain
**13.2.Rel1**, CMake ≥ 3.13, Ninja, a C++17 compiler, Python 3,
`libmbedtls-dev`, `libsdl2-dev`, `unzip`; Emscripten **3.1.74** for the
browser build.

| Gate | Command | Must |
|---|---|---|
| G-host | `PICO_SDK_PATH=… tools/gate.sh host` | build everything for the PC; `ctest` 100 % (ASan/UBSan) |
| G-fw | `PICO_SDK_PATH=… tools/gate.sh fw pico2_w pico2 pico_w pico` | every example links to `.uf2` on **all four** boards |
| G-web | `PICO_SDK_PATH=… tools/build_web_pods.sh` | `pd_demo` builds for the browser |

Logs: `build-gate/*.log`. Run G-web whenever you touch `pd::sys` timing,
`pd_net`, the host terminal, or `#ifdef __EMSCRIPTEN__` code.

**Also run the gates of every pod repository that uses what you changed**,
pointing it at this checkout: `PICOPODS_PATH=$PWD` from that repository
(`PICOPODS_PATH=/path/to/hydrogenium-pods tools/gate.sh all <its boards>`).
If you cannot (they are not checked out), say so in your report.

## Tests

- Add library tests to `tests/CMakeLists.txt` with
  `pd_host_test(<name> SOURCES <name>.cpp LIBS <pd_targets>)` (compiled with
  `-fsanitize=address,undefined -Wall`) and `add_test(...)`. Test data goes in
  `tests/data/`; Python checkers (`check_*.py`) are fine for file formats.
- `pd_demo` runs as a scripted session (`PD_KEYS`) through every screen;
  extend it when you add a UI facility the demo shows.
- Bluetooth tests are guarded by `PD_HAS_BT` and need `PICO_SDK_PATH`.
- Bug fix ⇒ regression test that fails before the fix. New API ⇒ test of it.
- Hardware-only code (drivers, DMA, flash) cannot be tested here: say so,
  and if useful add a check to `pd_demo`'s hardware screens.

Run a pod or example by hand:
`PD_SDCARD=./sd PD_FRAMES=./frames PD_KEYS="down,enter,text:hi,shot" ./build-gate/host/examples/demo/pd_demo`.
Key tokens: `enter esc bksp space up down left right f1…f12 ctrl+x shift+x`,
single characters, `text:<s>`, `wait:<ms>`, `shot`. Without `PD_KEYS` and with
SDL2, a window opens (F12 saves a screenshot).

## Documentation

- `lib/README.md`: one row per library (target, folder, headers, purpose).
- `docs/ARCHITECTURE.md`: layers, rules, HAL; update it when a rule or a
  layer changes. `docs/PORTING.md`: boards, terminals, host terminal,
  browser. `docs/ROADMAP.md` and `docs/MIGRATION.md` are history: append, do
  not rewrite.
- `README.md`: the family table, build commands, CI/release, "Write a pod".
  The Build / CI / Layout sections are parallel to the pod repositories'
  READMEs; keep the wording aligned.
- **Known naming drift**: the game is **PodCraft** (`pods/podcraft` in
  helium-pods). This repository's README, CHANGELOG, `docs/ROADMAP.md`,
  `docs/ARCHITECTURE.md` and `docs/MIGRATION.md` still call it *PicoCraft*.
  Fix it when you touch those files, keeping history accurate
  ("PicoCraft 1.7 … now PodCraft").
- Style: British spelling (colour, licence, behaviour), terse and exact,
  runnable code blocks with `/path/to/...` placeholders, Mermaid for diagrams.
- Old product names (`picodev`, `PicoBoy`, `PicoCalc Writer`, `PicoCalc
  Files`) only as provenance. Check:
  `git grep -n -i -E "picodev|picoboy|picocalc.?writer|picocalc.?files" -- . ':!docs/ROADMAP.md' ':!docs/MIGRATION.md'`
- Every command, option, path and target you document must be checked
  against the code.

### CHANGELOG.md
Newest first. Add to `## Unreleased`, under the existing sub-sections
(*Libraries*, *Build and CI*, *Repository split*…). Say what changed for a
pod author or a user, name the API (`pd::...`, `pd_<lib>`), and for fixes
the symptom (see the UF2 Loader entry for the expected depth).

## Changes that span repositories

- Use **the same branch name** here and in the pod repositories: their CI
  (`tools/ci_ref.sh`) builds against the hydrogenium-pods branch with the
  same name, so the pair is tested together.
- This repository lands **first**; the pod changes after it is on `main`.

## Git

- `main` is the released history. Work on the branch you are given; push to
  `main` only when explicitly told to. Open a pull request only when asked.
- Commit subject: `<component>: <imperative summary>` (`pd_fs: …`,
  `pd_ui: …`, `CI: …`, `Docs: …`), body wrapped, saying what and why and
  naming new tests. Add the attribution lines your harness requires. No
  model identifiers in commits, code or docs.
- Push with `git push -u origin <branch>`, retrying network errors with
  backoff.
- Never skip, disable or loosen a test or CI step to get green.

## Done means

- [ ] Change respects the architecture rules and compatibility above.
- [ ] Test added or extended (or a stated reason why none is possible).
- [ ] G-host, G-fw (4 boards), G-web where relevant: pass. Dependent pod
      repositories checked with `PICOPODS_PATH`, or reported as not checked.
- [ ] `CHANGELOG.md` and the affected docs updated and verified.
- [ ] Report: what changed (with `file:line`), gates run and results, what
      needs a check on real hardware, any follow-up for the pod repositories.
