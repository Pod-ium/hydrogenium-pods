#!/bin/sh
# CI helper: apt-get install with time limits and retries.
#
#   tools/ci_apt.sh <package...>
#
# On GitHub's runners the Ubuntu mirror is now and then so slow that an
# apt-get run takes hours; each attempt here is stopped after a few minutes
# and retried (packages already fetched are kept), and the man-db index
# rebuild (slow, not needed in CI) is turned off. Keep big downloads out of
# apt: the Arm toolchain comes from arm-none-eabi-gcc-action.
set -eu
sudo rm -f /var/lib/man-db/auto-update
for attempt in 1 2 3; do
    if sudo timeout 300 apt-get update -o Acquire::Retries=3 &&
       sudo DEBIAN_FRONTEND=noninteractive timeout 900 apt-get install -y -o Acquire::Retries=3 "$@"; then
        exit 0
    fi
    echo "== apt-get attempt $attempt failed or timed out"
    sudo dpkg --configure -a || true
    sleep 10
done
exit 1
