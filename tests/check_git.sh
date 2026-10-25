#!/bin/sh
# Clones real repositories with git_clone_test and checks each with the real
# git: object integrity (fsck), clean working tree, HEAD = the remote's.
#   check_git.sh <git_clone_test> <scratch-dir>
set -e
tool="$1"; root="$2"
command -v git >/dev/null || { echo "git not installed: skipped"; exit 0; }
run() {  # url depth [env]
    rm -rf "$root"; mkdir -p "$root"
    dir=$(env $3 "$tool" "$root" "$1" "$2") || { echo "clone failed: $1 depth=$2 $3"; exit 1; }
    repo="$root$dir"
    git -C "$repo" fsck --full --strict >/dev/null 2>&1 || { echo "fsck failed: $1"; git -C "$repo" fsck --full; exit 1; }
    [ -z "$(git -C "$repo" status --porcelain)" ] || { echo "working tree not clean: $1"; git -C "$repo" status --short | head; exit 1; }
    remote=$(git ls-remote "$1" HEAD | cut -f1)
    [ "$(git -C "$repo" rev-parse HEAD)" = "$remote" ] || { echo "HEAD differs from the remote: $1"; exit 1; }
    n=$(git -C "$repo" rev-list --count HEAD)
    echo "ok: $1 depth=$2 $3 ($n commits, $(git -C "$repo" ls-files | wc -l) files)"
}
run https://github.com/octocat/Hello-World 1
run https://github.com/octocat/Hello-World 0
run https://github.com/richgel999/miniz 1
run https://github.com/richgel999/miniz 1 PD_GIT_SMALL=1
run https://github.com/richgel999/miniz 0 PD_GIT_SMALL=1
echo "check_git: all ok"
