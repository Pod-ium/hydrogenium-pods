#!/bin/sh
# Builds this repository's pods for the board chosen (PicoCalc + Pico 2 W by
# default) and names the firmware files for hardware testing:  <pod>-test.uf2
#
#   tools/package_test.sh [outdir]        (PD_BOARD=pico2_w by default)
#
# Needs PICO_SDK_PATH. Files go to outdir (default dist-test/): one
# <pod>-test.uf2 per pod plus SHA256SUMS-test.txt. The "Pod test build"
# workflow (.github/workflows/pod-test.yml) runs this and attaches the files
# to the `test` pre-release of the repository.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/dist-test}
board=${PD_BOARD:-pico2_w}
build=${TEST_BUILD_DIR:-$root/build-test}
pods="pd_demo pd_usbprobe"   # the template is a skeleton to copy, not a firmware to flash

cmake -S "$root" -B "$build" -DPD_BOARD="$board" >/dev/null
# shellcheck disable=SC2086
cmake --build "$build" -j "$(nproc 2>/dev/null || echo 4)" --target $pods

mkdir -p "$out"
for p in $pods; do
    uf2=$(find "$build" -name "$p.uf2" -not -path '*/_deps/*' | head -n 1)
    [ -n "$uf2" ] || { echo "$p.uf2 not built" >&2; exit 1; }
    cp "$uf2" "$out/${p}-test.uf2"
done
(cd "$out" && sha256sum ./*-test.uf2 | sed 's| \./| |' > SHA256SUMS-test.txt && ls -l)
