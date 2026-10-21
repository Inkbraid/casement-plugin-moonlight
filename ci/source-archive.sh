#!/usr/bin/env bash
# Writes the complete source of the checked-out commit, submodules
# included, as one .tar.gz (GitHub's archives of a tag leave submodules
# out):
#
#   ci/source-archive.sh <name>.tar.gz
#
# Everything in the archive is under <name>/. The submodules must be
# checked out at the commits the repository records
# (git submodule update --init --recursive).
set -euo pipefail

if [ $# -ne 1 ] || [[ $1 != *.tar.gz ]]; then
    echo "usage: $0 <name>.tar.gz" >&2
    exit 2
fi
out=$(realpath -m "$1")
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if git submodule status --recursive | grep -q '^[-+U]'; then
    echo "submodules aren't checked out at the recorded commits:" >&2
    git submodule status --recursive >&2
    exit 1
fi

# git sets $name in submodule foreach; these have other names.
export ARCHIVE_PREFIX
ARCHIVE_PREFIX=$(basename "$out" .tar.gz)
export ARCHIVE_WORK
ARCHIVE_WORK=$(mktemp -d)
trap 'rm -rf "$ARCHIVE_WORK"' EXIT

git archive --format=tar --prefix="$ARCHIVE_PREFIX/" -o "$ARCHIVE_WORK/source.tar" HEAD
git submodule foreach --quiet --recursive '
    git archive --format=tar --prefix="$ARCHIVE_PREFIX/$displaypath/" -o "$ARCHIVE_WORK/submodule.tar" HEAD &&
    tar -Af "$ARCHIVE_WORK/source.tar" "$ARCHIVE_WORK/submodule.tar"'
gzip -9n <"$ARCHIVE_WORK/source.tar" >"$out"
echo "wrote $out"
