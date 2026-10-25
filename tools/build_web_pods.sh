#!/bin/sh
# Builds this repository's pods for the browser (PicoPods Web): one
# self-contained <pod>.pod.js per pod plus a pods.json listing them.
#
#   PICO_SDK_PATH=/path/to/pico-sdk tools/build_web_pods.sh [out-dir]
#
# Needs the Emscripten SDK on PATH (source emsdk_env.sh; tested with 3.1.74)
# and PICO_SDK_PATH for its mbedTLS sources. Output: default dist/web-pods.
# The page that runs them is in aurum-pods (web/); any .pod.js can also be
# loaded into a running PicoPods Web page like a cartridge.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/dist/web-pods}
build=${WEB_BUILD_DIR:-$root/build-web}
pods="pd_demo"   # the template and the USB probe are no use in a browser

command -v emcmake >/dev/null || { echo "emcmake not found: source emsdk_env.sh first"; exit 1; }
: "${PICO_SDK_PATH:?set PICO_SDK_PATH (the browser build uses its mbedTLS)}"

emcmake cmake -S "$root" -B "$build" -DPD_TERMINAL=host -DCMAKE_BUILD_TYPE=Release \
    -DPICO_SDK_PATH="$PICO_SDK_PATH" >/dev/null
# shellcheck disable=SC2086
cmake --build "$build" -j "$(nproc 2>/dev/null || echo 4)" --target $pods

rm -rf "$out"
mkdir -p "$out"
for p in $pods; do
    cp "$(find "$build" -name "$p.pod.js" -not -path '*/CMakeFiles/*' | head -n 1)" "$out"/
done
# pods.json: name, title, version (from picopods_add_pod) and file size.
python3 - "$build" "$out" $pods <<'PY'
import json, os, sys
build, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
pods = []
for n in names:
    meta = {"name": n, "title": n, "version": "?"}
    for dirpath, _, files in os.walk(build):
        if n + ".pod.json" in files:
            meta.update(json.load(open(os.path.join(dirpath, n + ".pod.json"))))
            break
    meta["size"] = os.path.getsize(os.path.join(out, n + ".pod.js"))
    pods.append(meta)
json.dump({"pods": pods}, open(os.path.join(out, "pods.json"), "w"), indent=1)
PY
echo "PicoPods Web pods: $out"
ls -l "$out"
