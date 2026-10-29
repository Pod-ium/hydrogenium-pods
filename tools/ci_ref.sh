#!/bin/sh
# CI helper: which ref of a sibling Pod-ium repository to build against.
#
#   tools/ci_ref.sh <owner/repo> <default-ref>
#
# A branch of the same name as the one being built (the PR's head branch, or
# the pushed branch) wins, so a change spanning several repositories is tested
# together; otherwise <default-ref>. Prints the ref.
set -eu
repo=$1; default=$2
branch=${GITHUB_HEAD_REF:-}
[ -n "$branch" ] || [ "${GITHUB_REF_TYPE:-}" != branch ] || branch=${GITHUB_REF_NAME:-}
if [ -n "$branch" ] && git ls-remote --exit-code --heads "https://github.com/$repo" "$branch" >/dev/null 2>&1; then
    echo "$branch"
else
    echo "$default"
fi
