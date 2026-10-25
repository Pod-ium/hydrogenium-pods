# Roadmap

How the PicoPods tree is put together from the original sources, as
milestones (M), stages (one branch + pull request each) and gates (what must
be true before a stage is merged).

## Sources

| Archive | Content |
|---|---|
| `picocalc-writer-2.1.0.zip` | PicoCalc Writer 2.1.0, standalone firmware with its own copy of the drivers |
| `picoboy-src.zip` | PicoBoy 1.0, standalone firmware with its own copy of the drivers |
| `picodev.zip` (older) | picodev 1: common libraries extracted from Writer and PicoBoy, both ported onto them |
| `picodev.zip` (newer) | picodev 2: adds PicoCalc Files, USB probe, Bluetooth, Git, TLS, USB drive |

## Branching

- `main`: released history. Only the integration branch merges into it.
- Integration branch (`claude/picocalc-firmware-reorganize-7lo2sb`): each stage
  branch is merged into it through its own pull request, with a merge
  commit, once its gate passes.
- Stage branches: `m<milestone>/<topic>`.

## Gates

| Gate | Check |
|---|---|
| **G-host** | `cmake -B build-host -DPD_TERMINAL=host -DPICO_SDK_PATH=…` builds with no errors and `ctest` passes 100% |
| **G-fw** | `cmake -B build` (PicoCalc + Pico 2 W) builds every firmware to `.uf2` |
| **G-names** | after M3: no old product name (`picodev`, `PicoBoy`, `PicoCalc Writer`, `PicoCalc Files`) outside history notes and compatibility shims |

G-names is checked by reading the output of
`git grep -n -i -E "picodev|picoboy|picocalc.?writer|picocalc.?files" -- . ':!docs/ROADMAP.md' ':!docs/MIGRATION.md'`:
what remains must be provenance ("from PicoCalc Writer's driver"), the
renamed-from notes in the pod READMEs, or the Pod Files folder migration.
| **G-ci** | after M4: the GitHub Actions workflow is green on the pull request |

## Milestones

### M0: Bootstrap
- [x] Repository skeleton, roadmap.

### M1: Import the original sources
Stage `m1/import-sources`. Gate: G-host, G-fw on the final snapshot.
Gate result: G-host 23/23 tests, G-fw pico2_w 6 `.uf2`.
- [x] PicoCalc Writer 2.1.0 into `legacy/picocalc-writer`
- [x] PicoBoy 1.0 into `legacy/picoboy`
- [x] picodev 1 at the root
- [x] picodev 2 on top (one diff commit)

### M2: Common libraries
Stage `m2/common-libs`. Gate: G-host, G-fw.
Gate result: G-host 23/23 tests, G-fw pico2_w / pico2 (6 `.uf2`), pico_w / pico (5, no PodBoy on RP2040).
- [x] Gate script (`tools/gate.sh`)
- [x] Build system renamed to PicoPods (`picopods_add_pod()`)
- [x] Libraries grouped by layer: `lib/hardware`, `lib/services`, `lib/interface`, `lib/content`
- [x] Library documentation

### M3: Pods
One stage per pod. Gate: G-host, G-fw, then G-names at the end.
Gate result: G-host 24/24 tests (new: `podfiles_migrate`), G-fw pico2_w: 3 pods + 3 examples, G-names clean.
- [x] `m3/podboy`: PicoBoy → **PodBoy** (`pods/podboy`)
- [x] `m3/pod-writer`: PicoCalc Writer → **Pod Writer** (`pods/podwriter`)
- [x] `m3/pod-files`: PicoCalc Files → **Pod Files** (`pods/podfiles`)
- [x] `m3/examples-cleanup`: demo, template, USB probe into `examples/`; retire `legacy/`

### M4: CI and release
Stage `m4/ci-release`. Gate: G-ci.
- [x] GitHub Actions: host tests and firmware for every board
- [x] Changelog, final docs
- [ ] Integration branch → `main`

### M5: one repository per family
The PicoPods tree is split into the Pod-ium repositories, each with its own
CI (G-host, G-fw, G-web on every push and pull request) and release workflow
(tag `v*` or a published release → `.uf2` files attached):

| Repository | Content |
|---|---|
| hydrogenium-pods | the libraries (`lib/`, `cmake/`), the examples, the library tests, these docs |
| helium-pods | games: PicoCraft |
| carbo-pods | productivity: Pod Writer, Pod Files |
| aurum-pods | emulators: PodBoy; PicoPods Web (`web/`, the site with every pod, GitHub Pages) |

The pod repositories find the libraries through `picopods_import.cmake`
(`PICOPODS_PATH`, or a git fetch of hydrogenium-pods). Gate result: G-host
19 (hydrogenium) + 5 (carbo) + 1 (helium) + 1 (aurum) = the 26 tests of the
single tree; G-fw unchanged per pod.
