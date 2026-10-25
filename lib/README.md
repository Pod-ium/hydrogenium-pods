# PicoPods libraries

The common code every pod is built on. Each library is a CMake `INTERFACE`
target compiled into the pod that links it (the Pico SDK convention), so
only what a pod uses ends up in its firmware. `picopods_add_pod()` always
links `pd_app`, which brings the terminal, `pd_ui`, `pd_gfx`, `pd_input`,
`pd_fs` and `pd_core`; anything else goes in `LIBS`.

The libraries are grouped in four layers. A layer only uses the layers
above it in this list, and only `lib/hardware` (plus the device backends
of `pd_net` and `pd_bt`) includes Pico SDK headers:
everything else is plain C++17 and is tested on a PC.

## `hardware/`: boards, terminals, drivers

| Target | Folder | Header(s) | What it gives you |
|---|---|---|---|
| `pd_core` | `core` | `pd/core.h`, `pd/sys.h`, `pd/board.h` | Colours, time, sleep, clock/overclock with voltage, core-1 launch (flash-safe), reboot/BOOTSEL, board capabilities |
| `pd_hal` | `hal` | `pd/hal/*.h` | Interfaces: `Display`, `KeyboardDevice`, `AudioOut`, `BlockDevice`, `Power` |
| `pd_drivers` | `drivers` | `pd/drivers/*.h` | ST7365P panel, SD over SPI (multi-block), PicoCalc keyboard/battery controller ("south bridge"), PWM stereo audio. Device builds only |
| `pd_terminal` | `terminals` | `pd/terminal.h`, `pd/terminals/*.h` | The selected terminal (`-DPD_TERMINAL=`): `picocalc` wires the drivers to the PicoCalc pins, `host` is a headless PC terminal for tests |
| `pd_input` | `input` | `pd/input.h`, `pd/keys.h` | Raw key layer (press/release, key state) for games, cooked layer (modifiers, auto-repeat) for UIs, optional polling on core 1 |
| `pd_diag` | `system` | `pd/diag.h` | Start-up log, reset breadcrumb, crash screen for HardFault and `panic()` |
| `pd_flash_cache` | `system` | `pd/flash_cache.h`, `pd/uf2loader.h` | Copy a file from SD into flash and read it memory-mapped; stays out of the PicoCalc UF2 Loader's flash (RP2350 partition, RP2040 top 16 KB) |
| `pd_usb_drive` | `usb` | `pd/usb_drive.h` | The SD card as a USB mass-storage device on the board's native USB (TinyUSB MSC) |

## `services/`: storage and connectivity

| Target | Folder | Header(s) | What it gives you |
|---|---|---|---|
| `pd_fs` | `fs` | `pd/fs.h`, `pd/path.h`, `pd/settings.h`, `pd/fs_ops.h` | Files and folders (FatFs R0.16 on the device, POSIX on the host), atomic writes, paths, `key=value` settings, recursive copy / move / delete with conflicts and progress |
| `pd_net` | `net` | `pd/net.h` | Wi-Fi scan/connect, DNS + mDNS, blocking TCP client (CYW43 + lwIP) |
| `pd_wifi` | `net` | `pd/wifi.h` | Saved credentials, network picker UI, connect on demand, idle power-off |
| `pd_bt` | `bt` | `pd/bt.h` | Bluetooth file transfer (OBEX Object Push) both ways, pairing keys on the SD card |
| `pd_tls` | `tls` | `pd/tls.h` | TLS client (mbedTLS 2.28) with built-in root CAs, HTTP/1.1 client |
| `pd_git` | `git` | `pd/git.h` | Git clone over HTTPS onto the SD card, with bounded RAM |
| `pd_miniz` | `miniz` | `miniz.h` | zlib inflate/deflate (miniz 3.0.2, third party) |
| `pd_crosspoint` | `crosspoint` | `pd/crosspoint.h` | Upload/delete/status for Xteink X3/X4 e-readers running CrossPoint |

`pd_net`, `pd_wifi`, `pd_crosspoint`, `pd_tls`, `pd_git` and `pd_bt` have a
real implementation only on Wi-Fi boards (and the host). CMake sets
`PD_HAS_WIFI`, `PD_HAS_TLS` and `PD_HAS_BT` so pods can link them
conditionally; on other boards they report "not available".

## `interface/`: drawing and user interface

| Target | Folder | Header(s) | What it gives you |
|---|---|---|---|
| `pd_gfx` | `gfx` | `pd/gfx.h`, `pd/blit.h` | 4 Terminus fonts (Latin-1), text, per-character colours, shapes, RGB blits; indexed framebuffer → scaled DMA blitter |
| `pd_ui` | `ui` | `pd/ui.h` | 3 themes, menus, message / confirm / prompt dialogs, progress, text view, file browser |
| `pd_app` | `app` | `pd/pd.h`, `pd/app.h` | Umbrella include, SD mount screen, splash, status bar with battery |

## `content/`: documents

| Target | Folder | Header(s) | What it gives you |
|---|---|---|---|
| `pd_text` | `text` | `pd/text/*.h` | Piece-table text buffer on the SD card (files larger than RAM, undo/redo), soft-wrap layout, streaming syntax lexer (Markdown, XML, Python, C/C++) |
| `pd_epub` | `epub` | `pd/epub.h` | Streaming EPUB 2.0.1 writer (Markdown → headings, lists, tables, code) |

See [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) for how the layers
fit together and [`docs/PORTING.md`](../docs/PORTING.md) to add a board
or a terminal.

## Licences

- FatFs (ChaN): BSD-style, `services/fs/fatfs/LICENSE.txt`.
- miniz: MIT, `services/miniz/LICENSE`.
- Terminus Font (Dimitar Toshkov Zhekov): SIL Open Font License 1.1.
- Display init sequence and pin map from ClockworkPi's reference code.
