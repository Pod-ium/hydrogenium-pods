#!/bin/sh
# CI helper: apt-get install with time limits and retries.
#
#   tools/ci_apt.sh <package...>
#
# On GitHub's runners an apt-get run now and then hangs for hours instead of
# failing; each attempt here is stopped after a few minutes and retried, and
# the man-db index rebuild (slow, not needed in CI) is turned off.
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
