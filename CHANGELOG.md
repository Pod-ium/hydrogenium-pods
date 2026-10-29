# Changelog

## Unreleased

### Repository split
- The PicoPods tree is split into the Pod-ium repositories: this one keeps
  the libraries, the examples, the library tests and the docs; the pods move
  to helium-pods (PodCraft), carbo-pods (Pod Writer, Pod Files) and
  aurum-pods (PodBoy, PicoPods Web). Their scripted host tests moved with them.
- `cmake/picopods_import.cmake`: pod repositories find the libraries through
  `PICOPODS_PATH` or fetch them with git (`PICOPODS_FETCH_FROM_GIT_TAG`).
- Each repository has its own CI (G-host, G-fw, G-web on pushes and pull
  requests) and release workflow; a pushed tag `v*` now creates the release.
- `tools/build_web.sh` became `tools/build_web_pods.sh` (this repository's
  pods only); the site build is `tools/build_web.sh` in aurum-pods.
  `tools/ci_ref.sh` picks the sibling repository's ref in CI.

### Libraries
- Host terminal: interactive SDL2 window for every pod (screen, live
  keyboard, sound; F12 saves a screenshot). Opens when `PD_KEYS` is not set;
  `PD_WINDOW`, `PD_SCALE`; built when SDL2 is found (`-DPD_HOST_SDL`).
  Headless runs and the tests are unchanged.

- PicoPods Web: the pods in a browser (the page is now in aurum-pods), with
  a touch PicoCalc keyboard, sound, an IndexedDB SD card and pod files that
  load like cartridges. `pd::sys` yields to the page from its sleeps and
  clock (Asyncify); `pd_net` has no Wi-Fi there.

### Build and CI
- G-host installs `libsdl2-dev`, so the window code is compiled on every push.
- G-web builds the browser pods on every push; PicoPods Web and its
  GitHub Pages workflow are now in aurum-pods.

## PicoPods 1.0.0 (2026-10-24)

First release of the PicoPods tree: the common PicoCalc libraries and three
pods, assembled from PicoCalc Writer 2.1.0, PicoBoy 1.0 and picodev 1–2
(see `docs/ROADMAP.md` for how, and `docs/MIGRATION.md` for the file map).

### Libraries
- picodev 2 renamed PicoPods: `cmake/picopods_init.cmake`,
  `cmake/picopods.cmake`, `picopods_add_pod()`. Targets (`pd_*`),
  headers (`pd/...`), the `pd::` namespace and `PD_*` options unchanged.
- `lib/` grouped by layer: `hardware/`, `services/`, `interface/`,
  `content/` (catalogue in `lib/README.md`).
- `pd::epub::Options::generator` (OPF generator metadata, default
  "PicoPods"); it was fixed to "PicoCalc Writer".
- Outward-facing identifiers say PicoPods: HTTP/Git user agent,
  USB drive vendor/manufacturer, default Bluetooth key file
  `/.picopods/bt_keys.bin`.

- UF2 Loader on RP2040: `pd_flash_cache` used the whole flash and could
  overwrite the loader in its top 16 KB (the demo's flash test on a Pico /
  Pico W). It now honours the loader's marker in the app's vector table and
  leaves the top 16 KB free even without it (`pd/uf2loader.h`, host-tested).
  The RP2350 path already stayed inside the app partition.

### Pods
- **PodBoy 1.1** (was PicoBoy 1.1): `pods/podboy`, `podboy.uf2`. Rename only.
- **Pod Writer 3.0.0** (was PicoCalc Writer 3.0.0): `pods/podwriter`,
  `podwriter.uf2`, Wi-Fi host name `PodWriter`, ePub generator
  "Pod Writer". Data stays in `/.fwedit/`.
- **Pod Files 1.3.0** (was PicoCalc Files 1.2.11): `pods/podfiles`,
  `podfiles.uf2`. Work folder `/.podfiles/`; an existing
  `/.picocalc_files/` is renamed on first start.

### Examples
- `pd_demo`, `pd_template`, `pd_usbprobe` moved to `examples/`, with
  PicoPods names; the demo's Wi-Fi file is `/.picopods/wifi.txt`.

### Build and CI
- `tools/gate.sh`: the merge gates (host build + tests, firmware build
  per board) in one command.
- GitHub Actions run both gates on every push and pull request and keep
  the `.uf2` files of each board as artifacts.
- Prebuilt `.uf2` files are no longer committed; take them from CI.
- Release workflow: every published release gets `podboy_<version>.uf2`,
  `podwriter_<version>.uf2`, `podfiles_<version>.uf2` and a SHA256SUMS file
  as release assets (`tools/package_release.sh` builds them locally too).
