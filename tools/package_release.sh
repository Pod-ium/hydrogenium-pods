#!/bin/sh
# Builds this repository's pods for the PicoCalc + Pico 2 W and names the
# firmware files for a release:  <pod>_<version>.uf2
#
#   tools/package_release.sh <version> [outdir]
#
# A leading "v" is dropped from the version (tag v1.2.0 -> pd_demo_1.2.0.uf2).
# Needs PICO_SDK_PATH. Files go to outdir (default dist/).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 <version> [outdir]" >&2; exit 2; }
version=${1#v}
out=${2:-$root/dist}
board=${PD_BOARD:-pico2_w}
build=${RELEASE_BUILD_DIR:-$root/build-release}
pods="pd_demo pd_usbprobe"   # the template is a skeleton to copy, not a firmware to flash

case $version in
    ''|*/*|*' '*) echo "bad version: '$1'" >&2; exit 2 ;;
esac

cmake -S "$root" -B "$build" -DPD_BOARD="$board" >/dev/null
# shellcheck disable=SC2086
cmake --build "$build" -j "$(nproc 2>/dev/null || echo 4)" --target $pods

mkdir -p "$out"
for p in $pods; do
    uf2=$(find "$build" -name "$p.uf2" -not -path '*/_deps/*' | head -n 1)
    [ -n "$uf2" ] || { echo "$p.uf2 not built" >&2; exit 1; }
    cp "$uf2" "$out/${p}_${version}.uf2"
done
(cd "$out" && sha256sum ./*_"$version".uf2 | sed 's| \./| |' > "SHA256SUMS_$version.txt" && ls -l)
